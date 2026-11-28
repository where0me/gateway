param([string]$NodeRoot=(Join-Path $PSScriptRoot '../../315-research'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'out/serial-comm-test-20261105/tests'
New-Item -ItemType Directory -Force $build|Out-Null
$env:ZIG_GLOBAL_CACHE_DIR="$root/out/zig-cache";$env:ZIG_LOCAL_CACHE_DIR="$root/out/zig-local"
$zig="$root/.codex-build-deps/ziglang/zig.exe"
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-function -UNDEBUG "-I$NodeRoot/Core/Inc" "$PSScriptRoot/test_node_comm_test.c" -o "$build/comm.exe"
if($LASTEXITCODE){throw 'Node comm test compilation failed'}
& "$build/comm.exe";if($LASTEXITCODE){throw 'Node comm test failed'}
$source=Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/test_node_console.c"
$actual=Get-Content -Raw -Encoding UTF8 "$NodeRoot/Core/Inc/node_console.h"
$source+=[regex]::Replace($actual,'(?m)^#include.*\r?\n','')
$source+=@'
static void feed(const char *text,int drain) {
    while(*text){fifo_read=0;fifo_count=0;while(*text && fifo_count<8)fifo[fifo_count++]=(uint8_t)*text++;
        in_irq=1;NodeConsole_UART_IRQHandler();in_irq=0;if(drain)NodeConsole_Process();}
}
int main(void) {
    NodeConsole_Init();assert(armed==2);
    feed("zbping",1);assert(!calls);feed("\r\n",1);assert(calls==1);
    feed("zbping\nzbping\r",0);assert(calls==1);NodeConsole_Process();assert(calls==3);
    feed("zbping\r\n",1);assert(calls==4); /* one complete 8-byte hardware FIFO burst */
    feed("bad command\n",1);assert(calls==4);
    feed("012345678901234567890123456789zbping\n",1);assert(calls==4);
    for(unsigned i=0;i<160;i++)feed("x",0);NodeConsole_Process();feed("\n",1);assert(calls==4 && node_console_ring_full>0);
    feed("zbping\n",1);assert(calls==5);
    feed("zbp",1);error_flags=15;in_irq=1;NodeConsole_UART_IRQHandler();in_irq=0;NodeConsole_Process();
    feed("ing\n",1);assert(calls==5);
    assert(!error_flags && node_console_overrun==1 && node_console_frame==1 && node_console_noise==1 && node_console_parity==1);
    feed("zbping\n",1);assert(calls==6 && armed>20 && scheduled>20 && hal_irqs>10 && rx_enabled && error_enabled);
    assert(strstr(output,"HEX=62 61 64") && strstr(output,"UART overrun=1 frame=1 noise=1 parity=1"));
    puts("Node console PASS: 8-byte FIFO burst, CR/LF/CRLF, exact commands, ring overflow and hardware error recovery/counters; no receive-rearm gaps or Zigbee/printing in IRQ");
}
'@
[IO.File]::WriteAllText("$build/console.c",$source,[Text.UTF8Encoding]::new($false))
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "$build/console.c" -o "$build/console.exe"
if($LASTEXITCODE){throw 'Node console compilation failed'}
& "$build/console.exe";if($LASTEXITCODE){throw 'Node console test failed'}
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -UNDEBUG "-I$root/gateway-core/include" "$PSScriptRoot/test_comm_test_gateway.c" -o "$build/gateway.exe"
if($LASTEXITCODE){throw 'Gateway diagnostic compilation failed'}
& "$build/gateway.exe";if($LASTEXITCODE){throw 'Gateway diagnostic test failed'}
