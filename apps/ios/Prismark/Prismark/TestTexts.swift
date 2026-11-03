// What each test means, in the same words as the desktop app.

struct TestText {
    let title: String
    let about: String
    let how: String
    let reading: String
}

let testTexts: [String: TestText] = [
    "s_st": TestText(
        title: "One core under stress",
        about: "How fast one core does vector maths after minutes of full load, and whether it stays correct. A single busy core keeps its highest clock only while the cooling and the power limits allow it.",
        how: "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed and clock are recorded every 5 seconds where the system allows it, and every result is checked against the first. The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes keeps its curve but has no score.",
        reading: "Higher is better, in GFLOP/s in the 10th minute."),
    "s_mc": TestText(
        title: "All cores under stress",
        about: "How fast every core together does vector maths after minutes of full load, when the processor is as hot as long use makes it, and whether every result stays correct. Phones slow down here as their power limits and cooling take over.",
        how: "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed and clock are recorded every 5 seconds where the system allows it, and every result is checked against the first. The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes keeps its curve but has no score.",
        reading: "Higher is better, in GFLOP/s in the 10th minute."),
    "s_cc": TestText(
        title: "Each core in turn",
        about: "Vector maths on a single core at its highest clock, moving to the next core every 10 seconds and around again, so a single weak core shows up.",
        how: "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed and clock are recorded every 5 seconds where the system allows it, and every result is checked against the first. The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes keeps its curve but has no score. The score is the slowest core's speed.",
        reading: "Higher is better, in GFLOP/s: the slowest core's speed."),
    "s_mem": TestText(
        title: "Memory under stress",
        about: "A memory test: every core writes most of the memory available and checks every word on the next pass, over and over, with four patterns in turn. It shows whether any memory comes back wrong when kept busy and warm.",
        how: "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed and clock are recorded every 5 seconds where the system allows it, and every result is checked against the first. The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes keeps its curve but has no score. Close other apps first.",
        reading: "Higher is better, in gigabytes per second (read and written) in the 10th minute. Any wrong word fails the test."),
    "mc_k2": TestText(
        title: "3D rendering on all cores",
        about: "How much rendering work the whole processor gets done when every core helps with the same image, as video and 3D rendering software do.",
        how: "A small ray tracer draws a fixed scene over and over, every core working on each frame. In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as slow) as long use makes it; then the test is measured for 20 seconds. Every device gets exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.",
        reading: "Higher is better. Measured in millions of light samples traced per second."
    ),
    "mc_k12": TestText(
        title: "Vector maths on all cores",
        about: "How many double-precision operations per second every core together gets done in dense matrix maths, as scientific, engineering and simulation software do. It uses the widest vector instructions the processor has (AVX-512, AVX2 or NEON), so it shows the processor's full floating-point power, and it is the hottest load in Prismark.",
        how: "Matrix multiplication in double precision, in tiles shared by every core, with fused multiply-adds as wide as the processor allows; every width gives exactly the same result. In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as slow) as long use makes it; then the test is measured for 20 seconds. Every device gets exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.",
        reading: "Higher is better, in billions of floating-point operations per second (GFLOP/s)."
    ),
    "st_k2": TestText(
        title: "3D rendering on one core",
        about: "How fast a single core is on a long task once it has warmed up: what one core can keep up.",
        how: "The renderer runs alone on the fastest core. In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as slow) as long use makes it; then the test is measured for 20 seconds. Every device gets exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.",
        reading: "Higher is better. Measured in millions of light samples traced per second."
    ),
    "st_k12": TestText(
        title: "Vector maths on one core",
        about: "How many double-precision operations per second a single core gets done in dense matrix maths, with the widest vector instructions it has (AVX-512, AVX2 or NEON).",
        how: "The matrix multiplication of “Vector maths on all cores”, on the fastest core alone. In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as slow) as long use makes it; then the test is measured for 20 seconds. Every device gets exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.",
        reading: "Higher is better, in billions of floating-point operations per second (GFLOP/s)."
    ),
    "b_k3": TestText(
        title: "Compressing a file",
        about: "How long one core takes to compress a small file with zstd, a common compression format. Short tasks like this are most of what everyday programs do.",
        how: "The task is repeated on a core that is already running, until its time is known to within about 1 %.",
        reading: "Lower is better, in milliseconds."
    ),
    "b_k4": TestText(
        title: "Opening a photo",
        about: "How long one core takes to open a JPEG photo and resize it, as a photo viewer or a web page does.",
        how: "The task is repeated on a core that is already running, until its time is known to within about 1 %.",
        reading: "Lower is better, in milliseconds."
    ),
    "b_k5": TestText(
        title: "Reading structured data (JSON)",
        about: "How long one core takes to read JSON documents, the text format most websites and online services exchange.",
        how: "The task is repeated on a core that is already running, until its time is known to within about 1 %.",
        reading: "Lower is better, in milliseconds."
    ),
    "b_k6": TestText(
        title: "Starting a small script",
        about: "How long it takes to start the Lua scripting language, load a script and run it, as when a plugin or a game script starts.",
        how: "The task is repeated on a core that is already running, until its time is known to within about 1 %.",
        reading: "Lower is better, in milliseconds."
    ),
    "m_lat1": TestText(
        title: "Memory delay",
        about: "How long the processor waits for one piece of data from main memory (RAM) when it is in none of the caches. Programs that jump around large amounts of data, such as databases and games, wait for this often.",
        how: "The processor follows a long chain of links through a block of memory larger than all its caches; each step has to wait for the previous one to arrive.",
        reading: "Lower is better, in nanoseconds (billionths of a second)."
    ),
    "m_latn": TestText(
        title: "Memory delay while every core uses memory",
        about: "The same wait while every other core is also reading main memory, so they all compete for it, as under heavy multitasking.",
        how: "The same chain of links, followed while every other core runs its own copy.",
        reading: "Lower is better, in nanoseconds. Compare with “Memory delay”: the difference is the cost of competition."
    ),
    "j_idle": TestText(
        title: "Timer punctuality on an idle device",
        about: "How late a task that asks to wake up every millisecond actually wakes up. It matters for audio, games and machine control, where late means a glitch.",
        how: "A task wakes up every millisecond for a minute (quick runs: shorter). The result is the lateness of its worst 1-in-1000 wake-ups, because an average would hide the rare long delays that cause glitches. Keep your hands off the screen while it runs.",
        reading: "Lower is better, in microseconds (millionths of a second)."
    ),
    "j_load": TestText(
        title: "Timer punctuality while other cores are busy",
        about: "The same timer while every other core is kept busy reading memory.",
        how: "As on an idle device, with every other core running a memory-heavy task.",
        reading: "Lower is better, in microseconds. Compare with “Timer punctuality on an idle device”."
    ),
    "r_1ms": TestText(
        title: "Reacting from rest: a 1 ms task",
        about: "An idle processor sleeps and slows down to save power. This shows how fast a short task runs when it arrives during that rest, compared with the same task on a processor that is already busy. You feel it as the small lag when you click something.",
        how: "Before each task the processor rests for a random 50–500 ms; then a task that takes 1 ms on a busy core is started and timed from the moment it was due. Keep your hands off the screen while it runs: a touch skips the test.",
        reading: "Higher is better. 100 % means no penalty; 50 % means the task took twice as long because the processor first had to wake up and speed up."
    ),
    "r_10ms": TestText(
        title: "Reacting from rest: a 10 ms task",
        about: "The same comparison for a task ten times longer. A longer task hides more of the wake-up delay, so this is usually closer to 100 %.",
        how: "As for the 1 ms task: a random rest of 50–500 ms, then a task that takes 10 ms on a busy core, timed from the moment it was due. Keep your hands off the screen while it runs.",
        reading: "Higher is better. 100 % means no penalty."
    ),
    "c_k4": TestText(
        title: "Opening a photo from rest",
        about: "How long opening a photo takes when the request arrives while the processor rests, wake-up delay included.",
        how: "Before each task the processor rests for a random 50–500 ms. The time is counted from the moment the task was due, so the wake-up delay is included. Keep your hands off the screen while it runs: a touch skips the test.",
        reading: "Lower is better, in milliseconds. Compare with “Opening a photo” under One core, measured on a busy core."
    ),
    "c_k6": TestText(
        title: "Starting a small script from rest",
        about: "How long starting a small script takes when the request arrives while the processor rests, wake-up delay included.",
        how: "Before each task the processor rests for a random 50–500 ms. The time is counted from the moment the task was due, so the wake-up delay is included. Keep your hands off the screen while it runs: a touch skips the test.",
        reading: "Lower is better, in milliseconds. Compare with “Starting a small script” under One core."
    ),
]
