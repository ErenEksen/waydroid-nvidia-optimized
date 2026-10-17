package dev.nvprolapse.hwuiapp;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import android.view.View;

/* hwuiapp: an ordinary Java activity whose only view animates every frame, so HWUI (debug.hwui.renderer,
 * skiavk in native mode) renders it continuously in this process. The APK carries an x86 native library
 * (libhwuiapp.so, empty) only so that the package manager runs it as a 32-bit process (P6). Logs the
 * process ABI and the frame rate (tag "hwuiapp"). */
public class MainActivity extends Activity {
    static {
        System.loadLibrary("hwuiapp");
    }

    static class Spinner extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private long t0 = System.nanoTime(), tl = t0;
        private int frames;

        Spinner(Context c) {
            super(c);
            paint.setTextSize(64f);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            long now = System.nanoTime();
            float t = (now - t0) / 1e9f;
            canvas.drawColor(Color.rgb(20, 30 + (int) (20 * Math.sin(t)), 60));
            float cx = getWidth() / 2f, cy = getHeight() / 2f, r = Math.min(cx, cy) * 0.6f;
            for (int i = 0; i < 12; i++) {
                double a = t * 2 + i * Math.PI / 6;
                paint.setColor(Color.HSVToColor(new float[] {(i * 30 + t * 60) % 360, 0.8f, 1f}));
                canvas.drawCircle(cx + (float) (r * Math.cos(a)), cy + (float) (r * Math.sin(a)), r * 0.15f, paint);
            }
            paint.setColor(Color.WHITE);
            canvas.drawText("hwuiapp " + (Process.is64Bit() ? "64-bit" : "32-bit") + " (HWUI)", 40, 120, paint);
            frames++;
            if (now - tl >= 2_000_000_000L) {
                Log.i("hwuiapp", frames + " frames in 2 s, " + (Process.is64Bit() ? "64-bit" : "32-bit")
                        + " process, hardware accelerated: " + canvas.isHardwareAccelerated());
                frames = 0;
                tl = now;
            }
            invalidate();
        }
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Log.i("hwuiapp", "onCreate: " + (Process.is64Bit() ? "64-bit" : "32-bit") + " process");
        setContentView(new Spinner(this));
    }
}
