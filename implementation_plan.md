# Edge Impulse Wake Word Integration Plan

We are going to replace the default Espressif "Hi Lexin" wake word with your custom "Sentinel" model from Edge Impulse, while keeping the Espressif MultiNet (command recognition) exactly as it is.

## User Review Required

> [!IMPORTANT]
> Since your trained model is tied to your personal Edge Impulse account, **you** need to export it to the project folder before I can write the code.

Please follow these steps:
1. Open your Edge Impulse project containing the classes "Sentinel", "Noise", and "Unknown" (or just your main project if you want to use that).
2. Go to the **Deployment** tab on the left menu.
3. Select **C++ Library** (do not select Arduino library or ESP-IDF specifically, just the raw C++ Library).
4. Click **Build** and download the `.zip` file.
5. Extract the contents of the `.zip` file into a new folder in your project root called: `components/edge_impulse` (so you should have `components/edge_impulse/edge-impulse-sdk`, etc.).
6. Reply to me letting me know when you have done this!

## Proposed Changes

Once you provide the library, I will write the code. Here is exactly what I will change:

### `main/CMakeLists.txt`
We will update the ESP-IDF build system to compile the Edge Impulse C++ source files alongside your project.

### `main/main.c`
We will rewrite the audio pipeline to support the "Hybrid" AI approach:
1. **Disable Espressif WakeNet**: We will change `afe_config->wakenet_init = false;` to free up significant RAM and CPU.
2. **Create `ei_task`**: A new FreeRTOS task dedicated to running the Edge Impulse `run_classifier_continuous` algorithm.
3. **Split Audio Feed**: We will modify `feed_task` to duplicate the incoming microphone audio: one copy goes to the AFE (Audio Front End) to prepare for commands, and the other goes into an Edge Impulse buffer.
4. **Custom Wake Event**: When Edge Impulse outputs a high confidence (e.g., > 80%) for the "Sentinel" class, we will trigger the exact same code that "Hi Lexin" used to trigger, instantly activating your existing command recognition logic!

## Verification Plan

1. Compile the project (this will take a while the first time because Edge Impulse compiles a lot of C++ files).
2. Flash the ESP32.
3. Say "Sentinel", check the serial monitor to see the confidence score, and then say a command to verify the hand-off worked.
