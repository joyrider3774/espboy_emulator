// How often timer1 (FRC1) interrupts with TIM_DIV1 and a load of 20000, at
// the CPU clock the sketch is built for. Prints "T1 <interrupts per second>".
volatile uint32_t count;

void IRAM_ATTR tick() { count++; }

void setup() {
  Serial.begin(115200);
  timer1_attachInterrupt(tick);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_LOOP);
  timer1_write(20000);
}

void loop() {
  uint32_t c0 = count, t0 = micros();
  delay(1000);
  uint32_t c1 = count, t1 = micros();
  Serial.printf("T1 %lu per s, cpu %d MHz\n", (unsigned long)((uint64_t)(c1 - c0) * 1000000 / (t1 - t0)), ESP.getCpuFreqMHz());
}
