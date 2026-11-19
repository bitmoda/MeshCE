#include <stdio.h>

/* The shared disc reader is built for the guest. The menu asks for an
extracted game folder, and says so when a disc image is chosen. */
int xiso_extract_maps(const char *image_path, const char *destination,
    void (*progress)(void *, const char *, unsigned long long, unsigned long long),
    void *context, char *error, int error_size)
{
    (void)image_path;
    (void)destination;
    (void)progress;
    (void)context;
    if (error && error_size > 0)
        snprintf(error, (size_t)error_size,
            "Choose the folder that already contains the maps directory.");
    return 0;
}

int xiso_extract_loading_picture(const char *image_path, const char *maps_directory,
    char *error, int error_size)
{
    (void)image_path;
    (void)maps_directory;
    if (error && error_size > 0)
        snprintf(error, (size_t)error_size,
            "This build does not extract the loading picture from the disc image.");
    return 0;
}
