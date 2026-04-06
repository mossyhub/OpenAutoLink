package com.openautolink.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import com.openautolink.app.data.AppPreferences
import com.openautolink.app.ui.navigation.AppNavHost
import com.openautolink.app.ui.projection.ProjectionViewModel
import com.openautolink.app.ui.theme.OpenAutoLinkTheme
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking

class MainActivity : ComponentActivity() {

    private val requiredPermissions = arrayOf(
        Manifest.permission.RECORD_AUDIO,
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.ACCESS_COARSE_LOCATION,
        Manifest.permission.POST_NOTIFICATIONS,
    )

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        val denied = results.filterValues { !it }.keys
        if (denied.isNotEmpty()) {
            Log.w("MainActivity", "Permissions denied: $denied")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        // Request runtime permissions on first launch
        requestMissingPermissions()

        // Apply saved display mode (sync read for initial state)
        val prefs = AppPreferences.getInstance(this)
        val displayMode = runBlocking { prefs.displayMode.first() }
        applyDisplayMode(displayMode)

        // Observe display mode changes reactively — applies immediately when
        // the user changes the setting, no app restart needed
        lifecycleScope.launch {
            prefs.displayMode.collectLatest { mode ->
                applyDisplayMode(mode)
            }
        }

        window.attributes.layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES

        setContent {
            OpenAutoLinkTheme {
                AppNavHost()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Display mode is now observed reactively — no need to re-read here
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // Log every KeyEvent for voice button investigation
        if (event.action == KeyEvent.ACTION_DOWN) {
            com.openautolink.app.diagnostics.DiagnosticLog.i(
                "input",
                "dispatchKeyEvent: keycode=${event.keyCode} (${KeyEvent.keyCodeToString(event.keyCode)}) action=DOWN source=0x${Integer.toHexString(event.source)}"
            )
        }
        val vm = ViewModelProvider(this)[ProjectionViewModel::class.java]
        if (vm.onKeyEvent(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        com.openautolink.app.diagnostics.DiagnosticLog.i(
            "input",
            "onKeyDown: keycode=$keyCode (${KeyEvent.keyCodeToString(keyCode)}) source=0x${Integer.toHexString(event.source)}"
        )
        return super.onKeyDown(keyCode, event)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        Log.i("MainActivity", "onNewIntent: action=${intent.action} extras=${intent.extras?.keySet()}")
        com.openautolink.app.diagnostics.DiagnosticLog.i(
            "input",
            "onNewIntent: action=${intent.action} extras=${intent.extras?.keySet()}"
        )
    }

    @Suppress("DEPRECATION") // Legacy flags needed for AAOS — WindowInsetsController alone is ignored
    private fun applyDisplayMode(mode: String) {
        Log.i("MainActivity", "applyDisplayMode: $mode")
        val controller = WindowCompat.getInsetsController(window, window.decorView)
        val decorView = window.decorView

        when (mode) {
            "system_ui_visible" -> {
                controller.show(WindowInsetsCompat.Type.systemBars())
                controller.systemBarsBehavior =
                    WindowInsetsControllerCompat.BEHAVIOR_DEFAULT
                window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
                @Suppress("DEPRECATION")
                decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_VISIBLE
            }
            "status_bar_hidden" -> {
                controller.hide(WindowInsetsCompat.Type.statusBars())
                controller.show(WindowInsetsCompat.Type.navigationBars())
                controller.systemBarsBehavior =
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                // Legacy fallback for AAOS where WindowInsetsController may be ignored
                window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
                @Suppress("DEPRECATION")
                decorView.systemUiVisibility = (
                    View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                )
            }
            "nav_bar_hidden" -> {
                controller.show(WindowInsetsCompat.Type.statusBars())
                controller.hide(WindowInsetsCompat.Type.navigationBars())
                controller.systemBarsBehavior =
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
                @Suppress("DEPRECATION")
                decorView.systemUiVisibility = (
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                )
            }
            "fullscreen_immersive" -> {
                controller.hide(WindowInsetsCompat.Type.systemBars())
                controller.systemBarsBehavior =
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                // Legacy fallback for AAOS where WindowInsetsController may be ignored
                window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
                @Suppress("DEPRECATION")
                decorView.systemUiVisibility = (
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                )
            }
            "custom_viewport" -> {
                // Custom viewport uses fullscreen immersive — the app handles viewport sizing
                controller.hide(WindowInsetsCompat.Type.systemBars())
                controller.systemBarsBehavior =
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
                @Suppress("DEPRECATION")
                decorView.systemUiVisibility = (
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                )
            }
        }
    }

    private fun requestMissingPermissions() {
        val missing = requiredPermissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            Log.i("MainActivity", "Requesting permissions: $missing")
            permissionLauncher.launch(missing.toTypedArray())
        }
    }
}
