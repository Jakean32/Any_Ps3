package com.anyps3.app;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.animation.DecelerateInterpolator;
import android.widget.ImageView;
import android.widget.TextView;

/**
 * Splash: the system shows the launcher icon (Android 12+ SplashScreen API
 * picks it up automatically from the adaptive icon). Here we continue with
 * the full-logo animation (fade + scale + settle) and route to the wizard
 * (first run) or the main shell.
 */
public class SplashActivity extends Activity {

    private static final long ANIM_MS = 1100;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_splash);

        final ImageView logo = findViewById(R.id.splash_logo);
        final TextView tagline = findViewById(R.id.splash_tagline);

        logo.setAlpha(0f);
        logo.setScaleX(0.82f);
        logo.setScaleY(0.82f);

        ObjectAnimator fadeIn = ObjectAnimator.ofFloat(logo, View.ALPHA, 0f, 1f);
        ObjectAnimator sx = ObjectAnimator.ofFloat(logo, View.SCALE_X, 0.82f, 1.03f, 1f);
        ObjectAnimator sy = ObjectAnimator.ofFloat(logo, View.SCALE_Y, 0.82f, 1.03f, 1f);
        ObjectAnimator tag = ObjectAnimator.ofFloat(tagline, View.ALPHA, 0f, 0.9f);

        AnimatorSet set = new AnimatorSet();
        set.play(fadeIn).with(sx).with(sy);
        set.play(tag).after(fadeIn);
        sx.setDuration(ANIM_MS);
        sy.setDuration(ANIM_MS);
        tag.setStartDelay(150);
        tag.setDuration(600);
        set.setInterpolator(new DecelerateInterpolator());
        set.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                route();
            }
        });
        set.start();
    }

    private void route() {
        Class<?> next;
        try {
            long h = AnyPs3App.get(this).library();
            String cfg = NativeBridge.nativeConfigJson(h);
            org.json.JSONObject o = new org.json.JSONObject(cfg);
            boolean wizardDone = o.optBoolean("wizard_done", false);
            String legal = o.optString("legal_version_accepted", "");
            next = (wizardDone && legal.equals(AnyPs3App.LEGAL_VERSION))
                    ? MainActivity.class : WizardActivity.class;
        } catch (Exception e) {
            next = WizardActivity.class;
        }
        startActivity(new Intent(this, next));
        overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
        finish();
    }
}
