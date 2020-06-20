#include "lua-timer.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#include <sys/time.h>
#include <mach/task.h>
#include <mach/mach.h>
#endif

#include "lua.h"
#include "lauxlib.h"

typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

struct timer_event {
	short rear;
	int64_t level;
	int64_t near;
};

struct timer_node {
	struct timer_node *next;
	uint32_t expire;
	uint64_t id;
};

struct link_list {
	struct timer_node head;
	struct timer_node *tail;
};

struct timer {
	struct link_list near[TIME_NEAR];
	struct link_list t[4][TIME_LEVEL];
	uint32_t time;
	uint32_t starttime;
	uint64_t current;
	uint64_t current_point;
	uint64_t cnt;
};

static inline struct timer_node *
link_clear(struct link_list *list) {
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

static inline void
link(struct link_list *list,struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

static inline void
add_node(struct timer *T,struct timer_node *node) {
	struct timer_event * tevent = (struct timer_event *) (node + 1);
	// 添加节点
	uint32_t time=node->expire;
	uint32_t current_time=T->time;
	
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		link(&T->near[time&TIME_NEAR_MASK],node);
		tevent->near = time&TIME_NEAR_MASK;
		// printf("near ----%d,%d,%d,%d\n",time,current_time,time|TIME_NEAR_MASK,current_time|TIME_NEAR_MASK);
	} else {
		int i;
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}
		int64_t level = (time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK;
		tevent->rear = i;
		tevent->level = level;
		// printf("REAR ----%d,%d,| %d，%d\n",time,current_time,level,i);
		link(&T->t[i][level],node);	
	}
}

static inline void *
timer_add(struct timer *T,void *arg,size_t sz,int time) {
	struct timer_node *node = (struct timer_node *)malloc(sizeof(*node)+sz);
	memcpy(node+1,arg,sz);
	node->expire=time+T->time;
	// printf(" add == %d, t->time=%d , expire =%d\n",time,T->time,node->expire);
	add_node(T,node);
	return node;
}

static inline void
move_list(struct timer *T, int level, int idx) {
	struct timer_node *current = link_clear(&T->t[level][idx]);
	while (current) {
		struct timer_node *temp=current->next;
		add_node(T,current);
		current=temp;
	}
}

static inline void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;
	uint32_t ct = ++T->time;
	if (ct == 0) {
		move_list(T, 3, 0);
	} else {
		uint32_t time = ct >> TIME_NEAR_SHIFT;
		int i=0;

		while ((ct & (mask-1))==0) {
			int idx=time & TIME_LEVEL_MASK;
			if (idx!=0) {
				move_list(T, i, idx);
				break;				
			}
			mask <<= TIME_LEVEL_SHIFT;
			time >>= TIME_LEVEL_SHIFT;
			++i;
		}
	}
}

static inline void
dispatch_list(lua_State *L,struct timer_node *current, int * tidx) {
	do {
		// 获取数据
		*tidx = *tidx + 1;
        lua_pushinteger(L,current->id);
        lua_rawseti(L, -2,*tidx); 
        // printf("dispatch tidx=%d=%d\n ",*tidx,current->id);
		
		struct timer_node * temp = current;
		current=current->next;
		free(temp);	
	} while (current);
}

static inline void
timer_execute(lua_State *L,struct timer *T,short * hasdata,int * tidx) {
	int idx = T->time & TIME_NEAR_MASK;
	// printf("exectue ==%d,idx=%d \n",T->time,idx);
	while (T->near[idx].head.next) {
		if( *hasdata == 0 )
		{
			*hasdata = 1;
			lua_newtable(L);
		}
		struct timer_node *current = link_clear(&T->near[idx]);
		dispatch_list(L,current,tidx);
	}
}

static inline struct timer *
timer_create_timer() {
	struct timer *r=(struct timer *)malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	r->current = 0;

	return r;
}

// centisecond: 1/100 second
static inline void
systime(uint32_t *sec, uint32_t *cs) {
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec;
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	*sec = tv.tv_sec;
	*cs = tv.tv_usec / 10000;
#endif
}

static inline uint64_t
gettime() {
	uint64_t t;
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint64_t)ti.tv_sec * 100;
	t += ti.tv_nsec / 10000000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (uint64_t)tv.tv_sec * 100;
	t += tv.tv_usec / 10000;
#endif
	return t;
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

uint64_t
skynet_thread_time(void) {
#if  !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	return (uint64_t)ti.tv_sec * MICROSEC + (uint64_t)ti.tv_nsec / (NANOSEC / MICROSEC);
#else
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t )&aTaskInfo, &aTaskInfoCount)) {
		return 0;
	}

	return (uint64_t)(aTaskInfo.user_time.seconds) + (uint64_t)aTaskInfo.user_time.microseconds;
#endif
}

static inline struct timer*
_to_timer(lua_State *L) {
    struct timer **t = lua_touserdata(L, 1);
    if(t==NULL) {
        luaL_error(L, "must be timer object");
    }
    return *t;
}

#define INVALID_VALUE -1
static inline int
_add(lua_State *L) {
    struct timer *pt = _to_timer(L);
    int second = luaL_checkinteger(L, 2);

    pt->cnt++;
    // 添加计时器
    struct timer_event event;
    // 初始赋值
    event.near = INVALID_VALUE;
	event.level = INVALID_VALUE;
    event.rear = INVALID_VALUE;

	struct timer_node * ptnode = timer_add(pt, &event, sizeof(event), second);
	ptnode->id = pt->cnt;//记录ID

    lua_pushinteger(L,pt->cnt);
    lua_pushlightuserdata(L,ptnode);
    return 2;
}

static inline int
_del(lua_State *L) {
    struct timer *pt = _to_timer(L);
    struct timer_node * ptnode = lua_touserdata(L, 2);
    struct timer_event * pevent = (struct timer_event *) (ptnode + 1);
    struct link_list *list = 0;
    if(pevent->near != INVALID_VALUE)
    {
    	list = &pt->near[pevent->near];
    }
    else if (pevent->rear != INVALID_VALUE && pevent->level != INVALID_VALUE)
    {
    	list = &pt->t[pevent->rear][pevent->level];
    }
    short found = 0;
    // 头节点
    struct timer_node *current = link_clear(list);
	while (current) {
		if(current == ptnode)
    	{ //去掉当前节点,并跳过当前节点,直接遍历下个节点
    		found = 1;
    		struct timer_node * temp = current->next;
    		struct timer_node * removenode = current;
    		free(removenode);
			current = temp;
    	}
    	else
    	{
    		struct timer_node *temp=current->next;
			add_node(pt,current);
			current = temp;
    	}
	}
    lua_pushboolean(L,found);
    return 1;
}

static int
_new(lua_State *L) {
    struct timer * pt = timer_create_timer();
    uint32_t current = 0;
	systime(&pt->starttime, &current);
	pt->current = current;
	pt->current_point = gettime();
	pt->cnt = 0;

    struct timer **t = (struct timer**) lua_newuserdata(L, sizeof(struct timer*));
    *t = pt;
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_setmetatable(L, -2);
    return 1;
}

static inline int
_release(lua_State *L) {
    struct timer *pt = _to_timer(L);
    // printf("collect pt:%p\n", pt);
    free(pt);
    return 0;
}

static inline int
_inc(lua_State *L) {
    struct timer *pt = _to_timer(L);
    pt->cnt++;
    lua_pushinteger(L,pt->cnt);
    return 1;
}

static inline int
_update(lua_State *L) {
    struct timer *pt = _to_timer(L);
    
	// 返回数据
	int tidx = 0;
    uint64_t cp = gettime();
	if(cp < pt->current_point) {
		// printf("time diff error: change from %lld to %lld", cp, pt->current_point);
		pt->current_point = cp;
	} else if (cp != pt->current_point) {
		uint32_t diff = (uint32_t)(cp - pt->current_point);
		pt->current_point = cp;
		pt->current += diff;
		// printf("current=%d,cpoint=%d,diff=%d,cp=%d\n",pt->current,pt->current_point,diff,cp);
		int i;
		short hasdata = 0;
		for (i=0;i<diff;i++) {
			timer_execute(L,pt,&hasdata,&tidx);
			timer_shift(pt);
			timer_execute(L,pt,&hasdata,&tidx);
		}
	}
	if(tidx == 0 )
	{
		lua_pushnil(L);
	}
    return 1;
}

int luaopen_shiftimer_c(lua_State *L) {
    luaL_checkversion(L);

    luaL_Reg l[] = {
        {"add", _add},
        {"del", _del},
        {"nextid", _inc},
        {"update", _update},
        {NULL, NULL}
    };

    lua_createtable(L, 0, 2);

    luaL_newlib(L, l);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, _release);
    lua_setfield(L, -2, "__gc");

    lua_pushcclosure(L, _new, 1);
    return 1;
}

