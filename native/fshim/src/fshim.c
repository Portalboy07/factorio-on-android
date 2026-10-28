/* Game loader: TLS setup, SDL3 dynapi override, dlopen and main(). */
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>

#include "fshim.h"
#include "game_constants.h"

#define TAG "fshim"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

uintptr_t fshim_game_base;
uintptr_t fshim_game_end;

struct base_search {
    const char *path;
    const char *soname;
    uintptr_t base;
    uintptr_t end;
};

static int find_base(struct dl_phdr_info *info, size_t size, void *opaque)
{
    (void)size;
    struct base_search *search = opaque;
    const char *name = info->dlpi_name ? info->dlpi_name : "";
    if ((search->path && strcmp(name, search->path) == 0) ||
        (search->soname && strstr(name, search->soname))) {
        search->base = (uintptr_t)info->dlpi_addr;
        for (int i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
            uintptr_t end = search->base + phdr->p_vaddr + phdr->p_memsz;
            if (phdr->p_type == PT_LOAD && end > search->end) {
                search->end = end;
            }
        }
        return 1;
    }
    return 0;
}

int fshim_run(const char *game_path, int argc, char **argv)
{
    const char *sdl = getenv("FSHIM_SDL");
    if (!sdl || !*sdl) {
        sdl = "libSDL3.so";
    }
    if (setenv("SDL3_DYNAMIC_API", sdl, 1) != 0) {
        LOGE("setenv(SDL3_DYNAMIC_API) failed: %s", strerror(errno));
    }
    LOGI("game image: %s", game_path);
    LOGI("SDL3_DYNAMIC_API=%s", sdl);

    /* Mesa panfrost mode: when the bundled Mesa GL stack is present next to
     * this shim, route the game onto real desktop GL (EGL_OPENGL_API +
     * libEGL_mesa/libGLESv2_mesa via SDL library-path hints) and take the
     * GLCompat layer out of the resolution path.  libgallium_dri.so is
     * preloaded by absolute path so libEGL_mesa.so's DT_NEEDED resolves. */
    {
        char libdir[512];
        char path[600];
        strncpy(libdir, game_path, sizeof(libdir) - 1);
        libdir[sizeof(libdir) - 1] = 0;
        char *slash = strrchr(libdir, '/');
        if (slash) {
            *slash = 0;
            snprintf(path, sizeof(path), "%s/libEGL_mesa.so", libdir);
            if (access(path, R_OK) == 0) {
                /* The app linker namespace blocks non-public vendor libs
                 * (libhardware).  Preload every dependency by absolute path
                 * so the DT_NEEDED entries resolve against already-loaded
                 * sonames. */
                /* Mesa's android stub libs ship bundled in the APK; preload
                 * them before libgallium_dri.so so its DT_NEEDED resolves. */
                const char *stubdeps[] = { "libhardware.so", "liblog.so",
                                           "libnativewindow.so", "libsync.so" };
                for (size_t i = 0; i < sizeof(stubdeps) / sizeof(stubdeps[0]); i++) {
                    if (!dlopen(stubdeps[i], RTLD_NOW | RTLD_GLOBAL)) {
                        LOGE("panfrost: preload %s failed: %s", stubdeps[i], dlerror());
                    }
                }
                snprintf(path, sizeof(path), "%s/libgallium_dri.so", libdir);
                void *gallium = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
                if (gallium) {
                    LOGI("panfrost: preloaded %s", path);
                } else {
                    LOGE("panfrost: gallium preload failed: %s", dlerror());
                }
                setenv("FACTORIO_MESA_EGL", "1", 1);
                snprintf(path, sizeof(path), "%s/libEGL_mesa.so", libdir);
                setenv("SDL_EGL_LIBRARY", path, 1);
                snprintf(path, sizeof(path), "%s/libGLESv2_mesa.so", libdir);
                setenv("SDL_OPENGL_LIBRARY", path, 1);
                LOGI("panfrost: FACTORIO_MESA_EGL=1 SDL_EGL_LIBRARY=%s",
                     getenv("SDL_EGL_LIBRARY"));
            }
        }
    }

    if (!fshim_install_tls()) {
        LOGE("TLS setup failed on the calling thread");
        return 70;
    }

    void *handle = dlopen(game_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        LOGE("dlopen failed: %s", dlerror());
        return 70;
    }

    struct base_search search = { .path = game_path,
                                  .soname = "libfactorio_game.so",
                                  .base = 0,
                                  .end = 0 };
    dl_iterate_phdr(find_base, &search);
    if (!search.base) {
        LOGE("cannot find the load base of %s", game_path);
        return 70;
    }

    fshim_game_base = search.base;
    fshim_game_end = search.end;

    int (*game_main)(int, char **) =
        (int (*)(int, char **))(search.base + FACTORIO_MAIN_VADDR);
    LOGI("game loaded: base %p, main %p, argc %d",
         (void *)search.base, (void *)game_main, argc);
    for (int i = 0; i < argc; i++) {
        LOGI("argv[%d] = %s", i, argv[i]);
    }

    int result = game_main(argc, argv);
    LOGI("game main returned %d", result);
    return result;
}
