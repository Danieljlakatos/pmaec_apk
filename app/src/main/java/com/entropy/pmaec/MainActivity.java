
package com.entropy.pmaec;

import android.app.Activity;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.ScrollView;
import android.widget.TextView;
import java.lang.ref.WeakReference;

public class MainActivity extends Activity {
    protected TextView logView;
    protected volatile boolean isRunning = true;
    protected AudioTrack audioTrack;
    protected static final int BUFFER_SIZE = 2048;
    protected Handler mainHandler;

    static {
        System.loadLibrary("pmaec_core");
    }

    public native int executePmaecPipeline(int[] outputBuffer, int[] maskBuffer);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ScrollView scrollView = new ScrollView(this);
        logView = new TextView(this);
        logView.setTextSize(14f);
        logView.setPadding(30, 30, 30, 30);
        logView.setBackgroundColor(0xFF101010);
        logView.setTextColor(0xFF00FF00);
        scrollView.addView(logView);
        setContentView(scrollView);

        logView.append("[PMAEC INITIALIZATION] Native Interface Bonded.\n");

        mainHandler = new Handler(Looper.getMainLooper());
        initAudioEngine();
        startTelemetryLoop();
    }

    private void initAudioEngine() {
        int minBufSize = AudioTrack.getMinBufferSize(
            44100,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        );
        audioTrack = new AudioTrack(
            AudioManager.STREAM_MUSIC,
            44100,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            Math.max(minBufSize, BUFFER_SIZE * 2),
            AudioTrack.MODE_STREAM
        );
        audioTrack.play();
    }

    private void startTelemetryLoop() {
        new Thread(new TelemetryHost(this)).start();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        isRunning = false;
        if (audioTrack != null) {
            try {
                audioTrack.stop();
                audioTrack.release();
            } catch (Exception e) {}
        }
    }

    private static class TelemetryHost implements Runnable {
        private final WeakReference<MainActivity> activityRef;

        TelemetryHost(MainActivity activity) {
            this.activityRef = new WeakReference<MainActivity>(activity);
        }

        @Override
        public void run() {
            int[] signalBuffer = new int[BUFFER_SIZE];
            int[] maskBuffer = new int[BUFFER_SIZE];
            short[] audioBuffer = new short[BUFFER_SIZE];

            while (true) {
                MainActivity activity = activityRef.get();
                if (activity == null || !activity.isRunning) {
                    break;
                }

                int faultStatus = activity.executePmaecPipeline(signalBuffer, maskBuffer);

                for (int i = 0; i < BUFFER_SIZE; i++) {
                    audioBuffer[i] = (short) (signalBuffer[i] & 0xFFFF);
                }

                if (activity.audioTrack != null && activity.isRunning) {
                    activity.audioTrack.write(audioBuffer, 0, BUFFER_SIZE);
                }

                String updateMessage = "[PMAEC RUNTIME] Vector 0: " + signalBuffer[0]
                    + " | Mask 0: " + maskBuffer[0]
                    + " | Trap State: " + (faultStatus != 0 ? "FALLBACK (" + faultStatus + ")" : "BARE-METAL ISA") + "\n";

                if (activity.isRunning) {
                    activity.mainHandler.post(new UIUploader(activity.logView, updateMessage));
                }

                try { Thread.sleep(22); } catch (InterruptedException e) { break; }
            }
        }
    }

    private static class UIUploader implements Runnable {
        private final TextView targetLog;
        private final String messageText;

        UIUploader(TextView logView, String message) {
            this.targetLog = logView;
            this.messageText = message;
        }

        @Override
        public void run() {
            if (targetLog != null) {
                targetLog.append(messageText);
                if (targetLog.getLineCount() > 40) {
                    targetLog.setText("");
                }
            }
        }
    }
}

