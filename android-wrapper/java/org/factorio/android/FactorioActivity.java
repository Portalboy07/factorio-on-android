package org.factorio.android;

import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/**
 * Factorio on Android.
 *
 * SDLActivity owns the surface, input, audio and the SDL thread; this class
 * only picks the native entry point (fshim_main in libfshim.so), the game
 * command line, and immersive fullscreen.
 */
public class FactorioActivity extends SDLActivity {
    private static final String TAG = "Factorio";

    @Override
    protected String[] getLibraries() {
        // The GL compatibility layer must be resident before SDL3 so that the
        // SDL_GL_GetProcAddress hook (fsgl_lookup) is already visible.  It is
        // only present in APKs packaged from a published build/shared rev.
        File glcompat = new File(getApplicationInfo().nativeLibraryDir, "libGLCompat.so");
        if (glcompat.exists()) {
            Log.i(TAG, "GL compatibility layer present: " + glcompat);
            return new String[] { "GLCompat", "SDL3", "fshim" };
        }
        return new String[] { "SDL3", "fshim" };
    }

    @Override
    protected String getMainFunction() {
        return "fshim_main";
    }

    /**
     * Copy the packaged canonical config.ini into the external files dir on
     * first run.  Factorio 2.1 rejects a hand-written config (the "invalid
     * contents" dialog) and then rewrites it; shipping its own reset file as
     * a template means a fresh install starts clean, with absolute
     * read-data/write-data paths substituted for this install.
     */
    private File externalFilesDir() {
        File ext = getExternalFilesDir(null);
        if (ext == null) {
            // Should not happen for a normal install; keep the game usable if
            // external storage is unavailable.
            ext = new File("/storage/emulated/0/Android/data/org.factorio.android/files");
        }
        return ext;
    }

    private void ensureFirstRunConfig(File ext) {
        File config = new File(ext, "config.ini");
        if (config.isFile() && config.length() > 0) {
            return;
        }
        // The runtime directories the template points at.
        new File(ext, "user").mkdirs();
        new File(ext, "mods").mkdirs();

        String template;
        try (InputStream in = getAssets().open("config.ini.template")) {
            ByteArrayOutputStream buf = new ByteArrayOutputStream(65536);
            byte[] chunk = new byte[8192];
            int n;
            while ((n = in.read(chunk)) > 0) {
                buf.write(chunk, 0, n);
            }
            template = buf.toString(StandardCharsets.UTF_8.name());
        } catch (IOException e) {
            Log.e(TAG, "cannot read config.ini.template from assets", e);
            return;
        }

        String base = ext.getAbsolutePath();
        String content = template
                .replace("@READ_DATA@", base + "/data")
                .replace("@WRITE_DATA@", base + "/user");

        File tmp = new File(ext, "config.ini.tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(content.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (IOException e) {
            Log.e(TAG, "cannot write " + tmp, e);
            return;
        }
        if (!tmp.renameTo(config)) {
            // A config appeared concurrently, or rename is unsupported.  Never
            // clobber an existing file: only fall back when it is still absent.
            if (config.isFile()) {
                Log.i(TAG, "config.ini appeared during first-run copy, keeping it");
                return;
            }
            try (FileOutputStream out = new FileOutputStream(config)) {
                out.write(content.getBytes(StandardCharsets.UTF_8));
            } catch (IOException e) {
                Log.e(TAG, "cannot write " + config, e);
                return;
            }
        }
        Log.i(TAG, "first-run config written: " + config + " (" + config.length() + " bytes)");
    }

    @Override
    protected String[] getArguments() {
        File ext = externalFilesDir();
        ensureFirstRunConfig(ext);
        File marker = new File(ext, "selftest");
        if (marker.exists()) {
            Log.i(TAG, "selftest marker present: " + marker);
            return new String[] { "--selftest" };
        }
        File core33 = new File(ext, "selftest-core33");
        if (core33.exists()) {
            Log.i(TAG, "core-profile selftest marker present: " + core33);
            return new String[] { "--selftest-core33" };
        }

        String base = ext.getAbsolutePath();
        Log.i(TAG, "game data dir: " + base);
        // The game resolves its install dir from /proc/self/exe, so every
        // path has to be absolute.  The shim already runs under this layout
        // in the shell harness.
        ArrayList<String> args = new ArrayList<>();
        args.add("--config");
        args.add(base + "/config.ini");
        args.add("--mod-directory");
        args.add(base + "/mods");
        // Optional extra arguments for testing, one per line, e.g.
        // "--benchmark-graphics" / "<path to save>"; delete the file to go
        // back to a normal start.
        File extra = new File(ext, "args.txt");
        if (extra.isFile()) {
            try (BufferedReader reader = new BufferedReader(new FileReader(extra))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty()) {
                        args.add(line);
                    }
                }
                Log.i(TAG, "extra arguments from " + extra + ": " + args.subList(4, args.size()));
            } catch (IOException e) {
                Log.e(TAG, "cannot read " + extra, e);
            }
        }
        return args.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Must exist before fshim_main parses --config, and before SDL starts.
        ensureFirstRunConfig(externalFilesDir());

        super.onCreate(savedInstanceState);

        // The desktop game is mouse-driven; SDL must synthesize mouse events
        // from finger input.  Hint names are the string values SDL looks up
        // (the SDL_HINT_* identifiers are C macro names, not the strings),
        // and they must be set after super.onCreate() has loaded the native
        // libraries but before the SDLThread starts reading them.
        SDLActivity.nativeSetenv("SDL_TOUCH_MOUSE_EVENTS", "1");
        SDLActivity.nativeSetenv("SDL_MOUSE_TOUCH_EVENTS", "0");

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemUi();
        // SDLActivity.onCreate() ends with setWindowStyle(false), which posts a
        // message that makes the system bars visible again.  Re-apply
        // immersive mode after that message has been handled.
        getWindow().getDecorView().postDelayed(new Runnable() {
            @Override
            public void run() {
                hideSystemUi();
            }
        }, 300);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUi();
        }
    }

    private void hideSystemUi() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
        window.getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }
}
