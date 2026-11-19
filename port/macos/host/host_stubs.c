/* Services the Android host provides. This Mac host does not run that app.
A zero result tells the guest the service is absent. */
int host_android_chip_family(void) { return 0; }
#if !defined(HALO_MACOS_NATIVE_METAL)
void host_metal_forget_warmup(void) {}
#endif
long long host_mcc_memory_reserve(unsigned int address, unsigned int size)
{
    (void)address;
    (void)size;
    return 0;
}
int host_mcc_memory_release(unsigned int address)
{
    (void)address;
    return 0;
}
int host_mcc_hash_start(void) { return 0; }
int host_mcc_hash_wait(void) { return 0; }
int host_mcc_hash_cancel(void) { return 0; }
int host_sha256_start(void) { return 0; }
int host_sha256_poll(void) { return 0; }
int host_sha256_cancel(void) { return 0; }
int host_touch_devices(void) { return 0; }
int host_steam_open(void) { return 0; }
int host_steam_close(void) { return 0; }
int host_steam_poll(void) { return 0; }
int host_steam_count(void) { return 0; }
int host_steam_view(void) { return 0; }
int host_steam_first(void) { return 0; }
int host_steam_set_first(void) { return 0; }
int host_steam_chosen(void) { return 0; }
int host_steam_set_chosen(void) { return 0; }
int host_steam_row(void) { return 0; }
int host_steam_details(void) { return 0; }
int host_steam_choose(void) { return 0; }
int host_steam_back(void) { return 0; }
int host_steam_picture(void) { return 0; }
int host_steam_text_wanted(void) { return 0; }
int host_steam_text_caption(void) { return 0; }
int host_steam_text_typed(void) { return 0; }
int host_steam_shared_fetch(void) { return 0; }
int host_steam_shared_state(void) { return 0; }
int host_steam_install_start(void) { return 0; }
int host_steam_install_status(void) { return 0; }
int host_steam_install_progress(void) { return 0; }
int host_steam_take_rescan(void) { return 0; }
int host_steam_author(void) { return 0; }
int posix_mcc_preview_read(const char *root, const char *relative, void *buffer, unsigned long capacity)
{
    (void)root;
    (void)relative;
    (void)buffer;
    (void)capacity;
    return 0;
}
int posix_mcc_share_open(const char *path)
{
    (void)path;
    return 0;
}
int posix_mcc_share_close(void) { return 0; }
int posix_mcc_share_path_safe(void) { return 0; }
int posix_mcc_share_publish(void) { return 0; }
int posix_mcc_share_remove(void) { return 0; }
