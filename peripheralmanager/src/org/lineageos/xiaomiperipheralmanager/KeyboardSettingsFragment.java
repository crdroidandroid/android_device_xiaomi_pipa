/*
 * Copyright (C) 2023-2025 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.xiaomiperipheralmanager;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.Log;

import android.preference.PreferenceManager;
import androidx.preference.PreferenceFragment;
import androidx.preference.SwitchPreference;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

import android.content.ComponentName;
import android.content.Intent;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

public class KeyboardSettingsFragment extends PreferenceFragment implements
        SharedPreferences.OnSharedPreferenceChangeListener {

    private static final String TAG = "XiaomiKeyboardSettings";
    private static final String KEYBOARD_KEY = "keyboard_switch_key";
    private static final boolean DEBUG = true;
    private static final String CONF_LOCATION = "/data/misc/xiaomi_keyboard.conf";

    private SharedPreferences mKeyboardPreference;

    private void saveAngleDetectionPreference(boolean enabled) {
    try {
        File file = new File(CONF_LOCATION);
        FileOutputStream fos = new FileOutputStream(file);
        fos.write((enabled ? "1" : "0").getBytes());
        fos.close();
        logInfo("Angle detection preference saved: " + enabled);
    } catch (IOException e) {
        logError("Failed to save angle detection preference: " + e.getMessage());
    }
}

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        try {
            addPreferencesFromResource(R.xml.keyboard_settings);

            mKeyboardPreference = PreferenceManager.getDefaultSharedPreferences(getContext());
            SwitchPreference switchPreference = (SwitchPreference) findPreference(KEYBOARD_KEY);

            if (switchPreference != null) {
                switchPreference.setChecked(mKeyboardPreference.getBoolean(KEYBOARD_KEY, false));
                switchPreference.setEnabled(true);
            } else {
                logError("Could not find keyboard switch preference");
            }

            logInfo("Keyboard settings fragment created");
        } catch (Exception e) {
            logError("Error creating keyboard settings: " + e.getMessage());
        }
    }

    @Override
    public void onResume() {
        super.onResume();
        try {
            mKeyboardPreference.registerOnSharedPreferenceChangeListener(this);
            logDebug("Registered preference change listener");

        } catch (Exception e) {
            logError("Error in onResume: " + e.getMessage());
        }
    }

    @Override
    public void onPause() {
        super.onPause();
        try {
            mKeyboardPreference.unregisterOnSharedPreferenceChangeListener(this);
            logDebug("Unregistered preference change listener");

        } catch (Exception e) {
            logError("Error in onPause: " + e.getMessage());
        }
    }

    @Override
    public void onSharedPreferenceChanged(SharedPreferences sharedPreference, String key) {
        if (KEYBOARD_KEY.equals(key)) {
            try {
                boolean newStatus = mKeyboardPreference.getBoolean(key, false);
                saveAngleDetectionPreference(newStatus);
                logInfo("Keyboard status changed: " + newStatus);
            } catch (Exception e) {
                logError("Error handling preference change: " + e.getMessage());
            }
        }
    }

    // Enhanced logging helpers to match other classes
    private void logDebug(String message) {
        if (DEBUG) Log.d(TAG, getTimestamp() + message);
    }

    private void logInfo(String message) {
        Log.i(TAG, getTimestamp() + message);
    }

    private void logError(String message) {
        Log.e(TAG, getTimestamp() + message);
    }

    private String getTimestamp() {
        return "[" + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date()) + "] ";
    }
}