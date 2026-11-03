/*
 * What each test means, in the same words as the desktop app (apps/gui/metrics.cpp).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

data class TestText(val title: String, val about: String, val how: String, val reading: String)

val testTexts = mapOf(
    "s_st" to TestText(
        "One core under stress",
        "How fast one core does vector maths after minutes of full load, and whether it stays correct. A single busy core keeps its highest clock only while the cooling and the power limits allow it.",
        "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed and clock are recorded every 5 seconds where the system allows it, and every result is checked against the first. The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes keeps its curve but has no score.",
        "Higher is better, in GFLOP/s in the 10th minute.",
    ),
    "s_mc" to TestText(
        "All cores under stress",
        "How fast every core together does vector maths after minutes of full load, when the processor is as hot as long use makes it, and whether every result stays correct. Phones slow down here as their power limits and cooling take over.",
        "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed and clock are recorded every 5 seconds where the system allows it, and every result is checked against the first. The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes keeps its curve but has no score.",
        "Higher is better, in GFLOP/s in the 10th minute.",
    ),
    "s_cc" to TestText(
        "Each core in turn",
        "Vector maths on a single core at its highest clock, moving to the next core every 10 seconds and around again, so a single weak core shows up.",
        "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed and clock are recorded every 5 seconds where the system allows it, and every result is checked against the first. The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes keeps its curve but has no score. The score is the slowest core's speed.",
        "Higher is better, in GFLOP/s: the slowest core's speed.",
    ),
    "s_mem" to TestText(
        "Memory under stress",
        "A memory test: every core writes most of the free memory and checks every word on the next pass, over and over, with four patterns in turn. It shows whether any memory comes back wrong when kept busy and warm.",
        "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed and clock are recorded every 5 seconds where the system allows it, and every result is checked against the first. The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes keeps its curve but has no score. Close other apps first.",
        "Higher is better, in gigabytes per second (read and written) in the 10th minute. Any wrong word fails the test.",
    ),
    "mc_k2" to TestText(
        "3D rendering on all cores",
        "How much rendering work the whole processor gets done when every core helps with the same image, as video and 3D rendering software do.",
        "A small ray tracer draws a fixed scene over and over, every core working on each frame. In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as slow) as long use makes it; then the test is measured for 20 seconds. Every device gets exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.",
        "Higher is better. Measured in millions of light samples traced per second.",
    ),
    "mc_k12" to TestText(
        "Vector maths on all cores",
        "How many double-precision operations per second every core together gets done in dense matrix maths, as scientific, engineering and simulation software do. It uses the widest vector instructions the processor has (AVX-512, AVX2 or NEON), so it shows the processor's full floating-point power, and it is the hottest load in Prismark.",
        "Matrix multiplication in double precision, in tiles shared by every core, with fused multiply-adds as wide as the processor allows; every width gives exactly the same result. In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as slow) as long use makes it; then the test is measured for 20 seconds. Every device gets exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.",
        "Higher is better, in billions of floating-point operations per second (GFLOP/s).",
    ),
    "st_k2" to TestText(
        "3D rendering on one core",
        "How fast a single core is on a long task once it has warmed up: what one core can keep up.",
        "The renderer runs alone on the fastest core. In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as slow) as long use makes it; then the test is measured for 20 seconds. Every device gets exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.",
        "Higher is better. Measured in millions of light samples traced per second.",
    ),
    "st_k12" to TestText(
        "Vector maths on one core",
        "How many double-precision operations per second a single core gets done in dense matrix maths, with the widest vector instructions it has (AVX-512, AVX2 or NEON).",
        "The matrix multiplication of “Vector maths on all cores”, on the fastest core alone. In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as slow) as long use makes it; then the test is measured for 20 seconds. Every device gets exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.",
        "Higher is better, in billions of floating-point operations per second (GFLOP/s).",
    ),
    "b_k3" to TestText(
        "Compressing a file",
        "How long one core takes to compress a small file with zstd, a common compression format. Short tasks like this are most of what everyday programs do.",
        "The task is repeated on a core that is already running, until its time is known to within about 1 %.",
        "Lower is better, in milliseconds.",
    ),
    "b_k4" to TestText(
        "Opening a photo",
        "How long one core takes to open a JPEG photo and resize it, as a photo viewer or a web page does.",
        "The task is repeated on a core that is already running, until its time is known to within about 1 %.",
        "Lower is better, in milliseconds.",
    ),
    "b_k5" to TestText(
        "Reading structured data (JSON)",
        "How long one core takes to read JSON documents, the text format most websites and online services exchange.",
        "The task is repeated on a core that is already running, until its time is known to within about 1 %.",
        "Lower is better, in milliseconds.",
    ),
    "b_k6" to TestText(
        "Starting a small script",
        "How long it takes to start the Lua scripting language, load a script and run it, as when a plugin or a game script starts.",
        "The task is repeated on a core that is already running, until its time is known to within about 1 %.",
        "Lower is better, in milliseconds.",
    ),
    "m_lat1" to TestText(
        "Memory delay",
        "How long the processor waits for one piece of data from main memory (RAM) when it is in none of the caches. Programs that jump around large amounts of data, such as databases and games, wait for this often.",
        "The processor follows a long chain of links through a block of memory larger than all its caches; each step has to wait for the previous one to arrive.",
        "Lower is better, in nanoseconds (billionths of a second).",
    ),
    "m_latn" to TestText(
        "Memory delay while every core uses memory",
        "The same wait while every other core is also reading main memory, so they all compete for it, as under heavy multitasking.",
        "The same chain of links, followed while every other core runs its own copy.",
        "Lower is better, in nanoseconds. Compare with “Memory delay”: the difference is the cost of competition.",
    ),
    "j_idle" to TestText(
        "Timer punctuality on an idle device",
        "How late a task that asks to wake up every millisecond actually wakes up. It matters for audio, games and machine control, where late means a glitch.",
        "A task wakes up every millisecond for a minute (quick runs: shorter). The result is the lateness of its worst 1-in-1000 wake-ups, because an average would hide the rare long delays that cause glitches. Keep your hands off the screen while it runs.",
        "Lower is better, in microseconds (millionths of a second).",
    ),
    "j_load" to TestText(
        "Timer punctuality while other cores are busy",
        "The same timer while every other core is kept busy reading memory.",
        "As on an idle device, with every other core running a memory-heavy task.",
        "Lower is better, in microseconds. Compare with “Timer punctuality on an idle device”.",
    ),
    "r_1ms" to TestText(
        "Reacting from rest: a 1 ms task",
        "An idle processor sleeps and slows down to save power. This shows how fast a short task runs when it arrives during that rest, compared with the same task on a processor that is already busy. You feel it as the small lag when you click something.",
        "Before each task the processor rests for a random 50–500 ms; then a task that takes 1 ms on a busy core is started and timed from the moment it was due. Keep your hands off the screen while it runs: a touch skips the test.",
        "Higher is better. 100 % means no penalty; 50 % means the task took twice as long because the processor first had to wake up and speed up.",
    ),
    "r_10ms" to TestText(
        "Reacting from rest: a 10 ms task",
        "The same comparison for a task ten times longer. A longer task hides more of the wake-up delay, so this is usually closer to 100 %.",
        "As for the 1 ms task: a random rest of 50–500 ms, then a task that takes 10 ms on a busy core, timed from the moment it was due. Keep your hands off the screen while it runs.",
        "Higher is better. 100 % means no penalty.",
    ),
    "c_k4" to TestText(
        "Opening a photo from rest",
        "How long opening a photo takes when the request arrives while the processor rests, wake-up delay included.",
        "Before each task the processor rests for a random 50–500 ms. The time is counted from the moment the task was due, so the wake-up delay is included. Keep your hands off the screen while it runs: a touch skips the test.",
        "Lower is better, in milliseconds. Compare with “Opening a photo” under One core, measured on a busy core.",
    ),
    "c_k6" to TestText(
        "Starting a small script from rest",
        "How long starting a small script takes when the request arrives while the processor rests, wake-up delay included.",
        "Before each task the processor rests for a random 50–500 ms. The time is counted from the moment the task was due, so the wake-up delay is included. Keep your hands off the screen while it runs: a touch skips the test.",
        "Lower is better, in milliseconds. Compare with “Starting a small script” under One core.",
    ),
)
