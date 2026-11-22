// Plus Modem: a Hayes-compatible Wi-Fi modem for the Macintosh Plus, with a built-in Ask AI relay.
// ESP32-WROOM-32 + MAX3232. See modem/README.md. The code lives in the .cpp files beside this one.
void boardSetup();
void boardLoop();

void setup() { boardSetup(); }
void loop() { boardLoop(); }
