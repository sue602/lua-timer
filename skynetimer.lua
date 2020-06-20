-- @Author: suyinxiang
-- @Date:   2020-06-02 09:22:20
-- @Last Modified by:   suyinxiang
-- @Last Modified time: 2020-06-15 15:04:02

local shiftimer = require "shiftimer.c"
local skynet = require("skynet")

local tinsert = table.insert
local tremove = table.remove

--- 状态
local state = {
    INIT = 0,
    RUNNING = 1,
    PAUSE = 2,
    STOP = 3,
}

local mt = {}
mt.__index = mt

--- 添加计时
-- @param ti,单位ti时间,默认为1/100秒
-- @param func,回调函数
-- @param args,回调函数的参数
-- @return true/false
function mt:add(ti, func, args)
    assert(type(func) == "function","timeout callback must be function")
    if ti < 0 then
        skynet.fork(func,args)
        return self.st:nextid()
    end
    local tid,node = self.st:add(ti)
    if tid and not self.tbl[tid] then
        self.tbl[tid] = {func=func,args=args,node=node}
        return tid
    end
    return false
end

--- 启动计时器
-- @return void
function mt:start()
    if self.state == state.INIT then
        self.state = state.RUNNING
        self.co = skynet.fork(self.__update,self)
    end
end

--- 暂停计时器
-- @return void
function mt:pause()
    if self.co and self.state ~= state.PAUSE then
        self.state = state.PAUSE
    end
end

--- 恢复
-- @return void
function mt:resume()
    if self.co and self.state == state.PAUSE then
        self.state = state.RUNNING
        skynet.wakeup(self.co)
    end
end

--- 停止计时器
-- @return void
function mt:stop()
    if self.co and self.state ~= state.STOP then
        self.state = state.STOP
        self.co = nil
    end
end

--- 删除计时
-- @param tid, 计时器ID
-- @return true/false
function mt:delete( tid )
    local tinfo = self.tbl[tid]
    if tinfo then
        local ok = self.st:del(tinfo.node)
        if ok then
            self.tbl[tid] = nil
        end
        return ok
    end
    return false
end

--- 设置计时
-- @param id, 计时器ID
-- @param newti, 新的ti时间,需要重新计算过,底层不负责重新计算ti时间
-- @return true/false
function mt:reset( tid, newti )
    local tinfo = self.tbl[tid]
    if tinfo then
        local ok = self.st:del(tinfo.node)
        if ok then
            local func = tinfo.func
            local args = tinfo.args
            self.tbl[tid] = nil
            if newti < 0 then
                skynet.fork(func,args)
                return self.st:nextid()
            end
            ok = false
            local newtid,node = self.st:add(newti)
            if newtid and not self.tbl[newtid] then
                self.tbl[newtid] = {func=tinfo.func,args=tinfo.args,node=node}
                return newtid
            end
        end
        return ok
    end
    return false
end


--- 计时器是否正在运行
-- @return true/false
function mt:isrun()
    return self.state == state.RUNNING
end

--- 每帧调用
-- @return void
function mt:__update()
    while true do
        if self.state == state.RUNNING then
            local tids = self.st:update()
            if tids then
                for _,tid in ipairs(tids) do
                    local tinfo = self.tbl[tid]
                    if tinfo and tinfo.func then
                        skynet.fork(tinfo.func, tinfo.args)
                    end
                    self.tbl[tid] = nil
                end
            end
            skynet.sleep(1)
        elseif self.state == state.STOP then
            break
        else
            skynet.wait(self.co)
        end
    end
    self.state = state.INIT
    print("timer update running=",self.state)
end

local M = {}
function M.new()
    local obj = {}
    obj.st = shiftimer()
    obj.tbl = {}
    obj.state = state.INIT
    return setmetatable(obj, mt)
end
return M

