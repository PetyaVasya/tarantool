local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new()
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

g.after_each(function(cg)
    cg.server:exec(function()
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

g.test_tombstone_threshold_invalid = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        t.assert_error_msg_contains('tombstone_threshold', function()
            s:create_index('pk', {tombstone_threshold = 1.1})
        end)
        t.assert_error_msg_contains('tombstone_threshold', function()
            s:create_index('pk', {tombstone_threshold = -0.1})
        end)
    end)
end

g.test_tombstone_threshold_create_and_options = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            tombstone_threshold = 0.35,
        })
        t.assert_equals(s.index.pk.options.tombstone_threshold, 0.35)
    end)
end

g.test_tombstone_threshold_default = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {run_count_per_level = 100})
        t.assert_equals(s.index.pk.options.tombstone_threshold, 1.0)
    end)
end
