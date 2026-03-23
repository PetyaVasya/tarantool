test_run = require('test_run').new()

--
-- DELETE ordinal histogram: optional run_info payload, disk.histogram_size,
-- memory.stmt_delete_histogram in box.stat.vinyl().
--

s = box.schema.space.create('hist', {engine = 'vinyl'})
pk = s:create_index('pk', {
    stmt_delete_histogram_max_bins = 64,
    page_size = 1024,
    range_size = 65536,
    run_count_per_level = 1,
})

for i = 1, 400 do
    s:replace{i, string.rep('y', 64)}
end
box.snapshot()

for i = 1, 150 do
    s:delete{i}
end
box.snapshot()

st = pk:stat()
st.disk.histogram_size > 0
box.stat.vinyl().memory.stmt_delete_histogram > 0

h1 = st.disk.histogram_size
m1 = box.stat.vinyl().memory.stmt_delete_histogram

test_run:cmd('restart server default')

s = box.space.hist
st2 = s.index.pk:stat()
st2.disk.histogram_size == h1
box.stat.vinyl().memory.stmt_delete_histogram == m1

s:drop()

--
-- Default (0): no histogram blob, histogram_size is zero.
--

s = box.schema.space.create('nohist', {engine = 'vinyl'})
_ = s:create_index('pk', {page_size = 1024, range_size = 65536})
s:replace{1, 'a'}
s:delete{1}
box.snapshot()
st = s.index.pk:stat()
st.disk.histogram_size == 0
s:drop()

test_run = nil
