typedef unsigned UInt;
static UInt Hwi_disable(void) { return 0; }
static void Hwi_restore(UInt key) { (void)key; }
