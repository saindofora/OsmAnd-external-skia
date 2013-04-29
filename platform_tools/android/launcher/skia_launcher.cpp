/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dlfcn.h>
#include <stdio.h>

void usage(const char* argv0) {
    printf("[USAGE] %s program_name [options]\n", argv0);
    printf("  program_name: the skia program you want to launch (e.g. tests, bench)\n");
    printf("  options: options specific to the program you are launching\n");
}

bool file_exists(const char* fileName) {
    FILE* file = fopen(fileName, "r");
    if (file) {
        fclose(file);
        return true;
    }
    return false;
}

int launch_app(int (*app_main)(int, const char**), int argc,
        const char** argv) {
    return (*app_main)(argc, argv);
}

int main(int argc, const char** argv) {

    // check that the program name was specified
    if (argc < 2) {
        printf("ERROR: No program_name was specified\n");
        usage(argv[0]);
        return -1;
    }

    // attempt to lookup the location of the skia app
    const char* appLocation = "/data/data/com.skia";
    if (!file_exists(appLocation)) {
        printf("ERROR: Unable to find the com.skia app on the device.\n");
        return -1;
    }

    // attempt to lookup the location of the shared libraries
    char libraryLocation[100];
    sprintf(libraryLocation, "%s/lib/lib%s.so", appLocation, argv[1]);
    if (!file_exists(libraryLocation)) {
        printf("ERROR: Unable to find the appropriate library in the Skia App.\n");
        printf("ERROR: Did you provide the correct program_name?\n");
        usage(argv[0]);
        return -1;
    }

    // load the appropriate library
    void* appLibrary = dlopen(libraryLocation, RTLD_LOCAL | RTLD_LAZY);
    if (!appLibrary) {
        printf("ERROR: Unable to open the shared library.\n");
        printf("ERROR: %s", dlerror());
        return -1;
    }

    // find the address of the main function
    int (*app_main)(int, const char**);
    *(void **) (&app_main) = dlsym(appLibrary, "main");

    if (!app_main) {
        printf("ERROR: Unable to load the main function of the selected program.\n");
        printf("ERROR: %s\n", dlerror());
        return -1;
    }

    // find the address of the SkPrintToConsole function
    void (*app_SkDebugToStdOut)(bool);
    *(void **) (&app_SkDebugToStdOut) = dlsym(appLibrary, "AndroidSkDebugToStdOut");

    if (app_SkDebugToStdOut) {
        (*app_SkDebugToStdOut)(true);
    } else {
        printf("WARNING: Unable to redirect output to the console.\n");
        printf("WARNING: %s\n", dlerror());
    }

    // pass all additional arguments to the main function
    return launch_app(app_main, argc - 1, ++argv);
}
