import re

text = open('/home/z/my-project/research/lv2.cpp').read()
start = text.find('g_ppu_syscall_table')
block = text[start:start + 60000]

# Table entries look like:  BIND_SYSC(sys_xxx),   // 403 (402)
nums = re.findall(r'BIND_SYSC\((\w+)\)\s*,?\s*//\s*(\d+)', block)
print(f"total entries with numbers: {len(nums)}")

found = {}
for name, num in nums:
    found.setdefault(name, num)

interesting = [
    'sys_tty_write', 'sys_tty_read',
    'sys_process_exit', 'sys_process_exit2', 'sys_process_getpid',
    'sys_fs_open', 'sys_fs_read', 'sys_fs_write', 'sys_fs_close',
    'sys_fs_opendir', 'sys_fs_readdir', 'sys_fs_stat', 'sys_fs_mkdir',
    'sys_fs_unlink', 'sys_fs_lseek', 'sys_fs_fstat', 'sys_fs_rename',
    'sys_memory_allocate', 'sys_memory_free', 'sys_memory_get_user_memory_size',
    'sys_ppu_thread_create', 'sys_ppu_thread_exit', 'sys_ppu_thread_join',
    'sys_ppu_thread_yield', 'sys_ppu_thread_detach',
    'sys_timer_usleep', 'sys_timer_sleep',
    'sys_time_get_current_time', 'sys_time_get_timebase_frequency',
    'sys_event_flag_create', 'sys_event_flag_wait', 'sys_event_flag_set',
    'sys_event_queue_create', 'sys_event_queue_receive', 'sys_event_port_send',
    'sys_prx_load_module', 'sys_prx_start_module',
    'sys_spu_thread_group_create', 'sys_spu_thread_group_start',
    'sys_spu_thread_create', 'sys_spu_thread_initialize', 'sys_spu_image_open',
    'sys_spu_thread_write_ls', 'sys_spu_thread_read_ls',
    'sys_mutex_create', 'sys_cond_create', 'sys_lwmutex_create',
    'sys_mmapper_allocate_shared_memory', 'sys_mmapper_map_shared_memory',
    'sys_game_process_exitspawn2', 'sys_process_atexit', 'sys_process_spawn',
]
for key in interesting:
    print(f"{key} = {found.get(key, '???')}")

# dump full sorted-by-number table to a file
table = sorted(((int(n), name) for name, n in found.items()))
with open('/home/z/my-project/research/syscall_table.txt', 'w') as f:
    for n, name in table:
        f.write(f"{n} {name}\n")
print("\nwrote syscall_table.txt with", len(table), "entries")
