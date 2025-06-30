/*
 * Copyright (C) 2023-2025 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.xiaomiperipheralmanager;

import android.os.Bundle;
import android.util.Log;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;

/**
 * Settings activity for stylus/pen configuration
 * Hosts the StylusSettingsFragment for user configuration
 */
public class KeyboardSettingsActivity extends CollapsingToolbarBaseActivity {

    private static final String TAG = "XiaomiKeyboardSettings";
    private static final String TAG_KEYBOARD = "keyboard";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        Log.i(TAG, "Opening keyboard settings");
        
        getFragmentManager().beginTransaction().replace(
            com.android.settingslib.collapsingtoolbar.R.id.content_frame,
                new KeyboardSettingsFragment(), TAG_KEYBOARD).commit();
    }
}
