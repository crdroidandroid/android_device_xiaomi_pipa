/*
 * Copyright (C) 2023 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.lineageos.xiaomiperipheralmanager;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.preference.PreferenceManager;
import androidx.preference.PreferenceFragment;
import androidx.preference.SwitchPreferenceCompat;
import com.android.settingslib.widget.FooterPreference;
import com.android.settingslib.widget.MainSwitchPreference;

public class StylusSettingsFragment extends PreferenceFragment implements
        SharedPreferences.OnSharedPreferenceChangeListener {
    private static final String TAG = "XiaomiPeripheralManagerPenUtils";
    private static final String STYLUS_MODE_KEY = "stylus_mode_key";
    private static final String FORCE_RECOGNIZE_STYLUS_KEY =
      "force_recognize_stylus_key";

    private SharedPreferences mStylusPreference;
    private RefreshUtils mRefreshUtils;

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        addPreferencesFromResource(R.xml.stylus_settings);

    Context context = getContext();
    mStylusPreference = PreferenceManager.getDefaultSharedPreferences(context);
    mRefreshUtils = new RefreshUtils(context);

    MainSwitchPreference stylusModePref =
        (MainSwitchPreference)findPreference("stylus_mode_key");
    stylusModePref.setChecked(
        mStylusPreference.getBoolean("stylus_mode_key", false));

    SwitchPreferenceCompat forceRecognizePref =
        (SwitchPreferenceCompat)findPreference("force_recognize_stylus_key");
    forceRecognizePref.setChecked(
        mStylusPreference.getBoolean("force_recognize_stylus_key", false));
  }

    @Override
    public void onResume() {
        super.onResume();
        mStylusPreference.registerOnSharedPreferenceChangeListener(this);
    }

    @Override
    public void onPause() {
        super.onPause();
        mStylusPreference.unregisterOnSharedPreferenceChangeListener(this);
    }

    @Override
    public void onSharedPreferenceChanged(SharedPreferences sharedPreferences,
                                          String key) {
        if (STYLUS_MODE_KEY.equals(key)) {
          setStylusMode(sharedPreferences.getBoolean(key, false));
        } else if (FORCE_RECOGNIZE_STYLUS_KEY.equals(key)) {
          setForceRecognizeStylus(sharedPreferences.getBoolean(key, false));
        }
    }

    private void setStylusMode(boolean enabled) {
      if (enabled)
        mRefreshUtils.setPenRefreshRate();
      else 
        mRefreshUtils.setDefaultRefreshRate();
    }

    private void setForceRecognizeStylus(boolean enabled) {
      if (enabled)
        PenUtils.enablePenMode();
      else 
        PenUtils.disablePenMode();
    }
}
