/*
 * What this device is, how it is right now, and what Android will not tell an app. The status is read when the
 * screen opens and again when the system says it changed; there is no timer.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private class DeviceStatus(val heat: String, val hot: Boolean, val lowPower: Boolean, val battery: String, val batteryNote: String)

private fun readStatus(context: Context): DeviceStatus {
    val power = context.getSystemService(Context.POWER_SERVICE) as PowerManager
    val thermal = if (Build.VERSION.SDK_INT >= 29) power.currentThermalStatus else -1
    val heat = when (thermal) {
        -1 -> "Unknown"
        0 -> "Nominal"
        1 -> "Light"
        2 -> "Moderate"
        3 -> "Severe"
        4 -> "Critical"
        else -> "Shutdown"
    }
    val sticky = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    val level = sticky?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
    val scale = sticky?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
    val note = when (sticky?.getIntExtra(BatteryManager.EXTRA_STATUS, -1)) {
        BatteryManager.BATTERY_STATUS_CHARGING -> "Charging"
        BatteryManager.BATTERY_STATUS_FULL -> "Full, plugged in"
        BatteryManager.BATTERY_STATUS_DISCHARGING, BatteryManager.BATTERY_STATUS_NOT_CHARGING -> "On battery"
        else -> "Not reported"
    }
    return DeviceStatus(
        heat, thermal >= 3, power.isPowerSaveMode,
        if (level < 0 || scale <= 0) "—" else "${level * 100 / scale} %", note,
    )
}

@Composable
fun DeviceScreen(tablet: Boolean) {
    val context = LocalContext.current
    val machine = remember(DeviceInfo.doc) { Machine(context, DeviceInfo.doc) }
    var status by remember { mutableStateOf(readStatus(context)) }

    DisposableEffect(Unit) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                status = readStatus(c)
            }
        }
        val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED).apply { addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED) }
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else context.registerReceiver(receiver, filter)
        val power = context.getSystemService(Context.POWER_SERVICE) as PowerManager
        val thermal = if (Build.VERSION.SDK_INT >= 29) PowerManager.OnThermalStatusChangedListener { status = readStatus(context) } else null
        if (Build.VERSION.SDK_INT >= 29 && thermal != null) power.addThermalStatusListener(thermal)
        onDispose {
            context.unregisterReceiver(receiver)
            if (Build.VERSION.SDK_INT >= 29 && thermal != null) power.removeThermalStatusListener(thermal)
        }
    }

    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Column(
            Modifier.fillMaxWidth().background(Signal).drawBehind { passDots() }.padding(top = topInset() + 12.dp, start = 20.dp, end = 20.dp, bottom = 10.dp),
        ) {
            Mono("This device · ${machine.model}", OnSignal)
            ChipName(machine, if (tablet) 130f else 84f)
            BasicText("${machine.system} · ${machine.cores}", style = Type.archivo(13.sp, 600, OnSignal))
        }
        StatusTiles(status, if (tablet) 50.sp else 30.sp)
        CoreMap(machine, Modifier.weight(1f).fillMaxWidth(), if (tablet) 84.sp else 50.sp, if (tablet) 56.sp else 24.sp, if (tablet) 64.dp else 46.dp)
        Column(Modifier.fillMaxWidth().background(LocalPalette.current.raised).padding(horizontal = 16.dp, vertical = 9.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Mono("Not readable", LocalPalette.current.muted)
            BasicText(
                "Temperature in degrees, clock speed, core placement and the cycle counter are closed to apps on many phones. Each result file records what was unknown.",
                style = Type.archivo(12.sp, color = LocalPalette.current.muted),
            )
        }
        val bottom = bottomInset()
        Row(
            Modifier.fillMaxWidth().height(56.dp + bottom).background(Signal)
                .tap { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://prismark.leonardonels.com/compare.html"))) }
                .padding(start = 20.dp, end = 20.dp, bottom = bottom),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            BasicText("Compare processors on the web", style = Type.archivo(15.sp, 600, OnSignal), modifier = Modifier.weight(1f))
            BasicText("↗", style = Type.archivo(17.sp, 600, OnSignal))
        }
    }
}

@Composable
private fun StatusTiles(status: DeviceStatus, size: TextUnit) {
    // As tall as the tallest tile, so a larger text size on the phone never cuts the words off.
    Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Tile("Heat", status.heat, "As Android reports it", status.hot, size, Modifier.weight(1f))
        Tile("Low Power", if (status.lowPower) "On" else "Off", "Slows the CPU when on", status.lowPower, size, Modifier.weight(1f))
        Tile("Battery", status.battery, status.batteryNote, false, size, Modifier.weight(1f))
    }
}

@Composable
private fun Tile(name: String, value: String, note: String, alert: Boolean, size: TextUnit, modifier: Modifier) {
    val p = LocalPalette.current
    Column(modifier.fillMaxHeight().background(p.panel).padding(horizontal = 10.dp, vertical = 9.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Mono(name, p.muted)
        BasicText(value, style = Type.saira(size, 50, 600, if (alert) Fault else p.text), maxLines = 1)
        BasicText(note, style = Type.archivo(12.sp, color = p.muted), minLines = 2, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

/** The processor drawn as it is: one tile per core, cluster by cluster, then what the cores share. */
@Composable
private fun CoreMap(machine: Machine, modifier: Modifier, fastSize: TextUnit, otherSize: TextUnit, bandHeight: Dp) {
    val p = LocalPalette.current
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        machine.clusters.forEachIndexed { index, (name, count) ->
            val first = index == 0
            Row(
                Modifier.fillMaxWidth().weight(if (first) 2f else 1f),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                repeat(count) { core ->
                    Column(
                        Modifier.weight(1f).fillMaxHeight().background(if (first) Volt else p.steel).padding(horizontal = 8.dp, vertical = 8.dp),
                        verticalArrangement = Arrangement.SpaceBetween,
                    ) {
                        val text = if (first) Ink else p.text
                        BasicText(if (count > 4 && name.length > 4) name.take(3).uppercase() else name.uppercase(), style = Type.mono(10.sp, color = text), maxLines = 1)
                        BasicText("${core + 1}", style = Type.saira(if (first) fastSize else otherSize, 50, 600, text), maxLines = 1)
                    }
                }
            }
        }
        machine.cache?.let { Band("Shared cache", it, p.raised, bandHeight) }
        Band("Memory", machine.memory, p.panel, bandHeight)
    }
}

@Composable
private fun Band(name: String, value: String, fill: Color, height: Dp) {
    Row(Modifier.fillMaxWidth().height(height).background(fill).padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
        Mono(name)
        Spacer(Modifier.weight(1f))
        BasicText(value, style = Type.saira(28.sp, 50, 600))
    }
}
