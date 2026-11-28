# CC1352R coordinator gateway port

Source behavior: STM32-Projects-1.91/Zigbee-coordinator (gateway_bridge.c).
Target: SimpleLink SDK 8.32.00.07, CC1352R1, TI-RTOS7.

## Hardware and build

- UART0: DIO13 TX / DIO12 RX, 115200, 8N1, no flow control.
- TXPOWER = +5 dBm. RF design: differential external bias, CC1352R1 reference.
- External 48 MHz and 32.768 kHz crystals.
- No buttons, LEDs, external flash, SPI or terminal UI.
- Internal NVS at 0x52000, length 0x4000; NV_INIT/NV_RESTORE retained.
- RESET_N remains hardware reset; boot does not factory-reset the network.
- Zigbee application endpoint 8, HA profile 0x0104; clients Basic 0x0000 and
  Temperature Measurement 0x0402. Sensor endpoint 1 is unchanged.
- UART output is binary protocol only, not readable serial-console text.

## Project-local changes

The inspected .project has no linkedResources: the SDK sources were already
copied into this project. No file under C:/ti is edited by this port.

Modified:
- zc_genericapp.syscfg: custom board/UART2, +5 dBm, remove absent peripherals.
- .cproject: CUI_DISABLE; exclude the original Application/zcl_genericapp.c,
  the unused CUI implementation and OTA source (no OTA backend in this port).
- Stack/ZStackTask/zstack.h: add nwkUpdateId to the local network-info response.
- Stack/ZStackTask/zstacktask.c: read nwkUpdateId in the stack thread.

Added:
- Application/gateway_entry.c: sampleApp_task entry point.
- Application/gateway_bridge.c: UART/Zigbee bridge and coordinator task.
- Application/gateway_bridge.h: entry-point declaration.
- Application/gateway_protocol.h: portable binary encoder/parser.
- PORTING_NOTES.md: this manifest and verification instructions.

Original genericapp source is preserved, but not compiled. Existing unused
genericapp attribute data remains unchanged. Modified originals are backed up
outside the project under workspace_ccstheia/cc1352r-backups.

## Protocol coverage

Same UART envelope: FE CMD LEN PAYLOAD BCC; BCC is XOR of CMD/LEN/PAYLOAD.
Little-endian integers and EUI64 byte order match ST/Linux.

Commands: 01/03 open join, 05 close, 02/11 remove, 04 model read, 07
temperature read, 08 custom action, 0A network info, 0B/0C monitoring/config,
0D V2 waveform ACK, 10 probe, 0E channel migration, 12 neighbor snapshot.

Responses: 83 scan submission, 84 announce, 85 model, 86-89 measurements,
8A network, 8B/8C vibration, 8D state (including power/LQI/RSSI), 8E submission,
8F Leave, 90 AF confirm, 92-95 V2 stream, 96 channel submission, 98 ready,
99 config query, 9A restore, 9B probe, 9C cleanup submission, 9D/9E snapshot,
A0 recovery activity. Recovery commands 11/12 are answered independently of
the Linux application's authorization decisions.

## Deliberate limitations / required hardware verification

- This is an initial hardware-unverified port, not a claim of production parity.
- TI Mgmt_Lqi has no RSSI: snapshot reports -128 rather than a made-up value.
- ST-specific security-table diagnostics are not emulated. Only equivalent
  announce/Leave/overflow events use 9F. The TI security stack is retained.
- Successful child LeaveCnf and non-rejoin remote LeaveInd now both run the
  official cleanup path in the stack task, with NV write/readback auditing.
  Failed child confirmations do not remove local records. A submission result
  is never a completion. Neither callback proves remote application-flash erase.
- Extended 8F completion requires a matching removal slot, successful TC/APP
  cleanup and verified NV writes. The backend still requires all three statuses
  zero and complete=1. No manual NWK frame-counter reset is performed.
- An NV fault is latched for this boot: subsequent duplicate Leave events cannot
  hide an earlier ignored write failure. Backend tasks remain pending. Diagnose
  flash/power errors before retrying after restart; this is not an automatic
  power-loss-recovery journal or a guarantee against lost UART completion frames.
- Temperature read sends the standard read request, as ST did; no new invented
  serial format is introduced for the standard attribute response.
- Source=restored means TI restored its own NV; ST network snapshots cannot be
  imported into TI. Recommission nodes into the new TI network for first test.
- Channel response is submission status only; exercise prepare/commit with
  sleeping nodes before enabling this in deployment.

## Permit-join diagnostic messages

The binary 9F envelope is retained; no printf text is mixed into UART.
TI-specific events: 30 = request submission (duration/status/source command),
31 = official DEV_PERMIT_JOIN_IND (window duration, not remaining-time readback),
32 = MGMT_PERMIT_JOIN_RSP (source/status). Event 11 reports a request blocked
because the coordinator is not ready. Open, close, startup-close and automatic
renewal are covered. Linux main.c decodes these events for terminal and database
logs. ST-only trust-policy and key-table diagnostics are not fabricated.

## Verification on 2026-10-07

- CCS default full compile/link passed: 0 errors. Output .out and .hex exist.
- One tool-version warning: SysConfig 1.27.0 replaces requested 1.21.1.
- Generated settings checked: TXPOWER=5, UART0 DIO13 TX/DIO12 RX, internal NVS
  only. The SDK installation was not edited.
- AF transaction correlation keeps the original ID before TI increments it.
- UART receive overflow discards the truncated stream, rather than dispatching
  partial old commands. Payload lengths are bounded by the FE frame protocol.
- Syntax checks of the bridge and changed stack task passed.
- Portable exhaustive protocol test source is staged, but was not run because
  no host C compiler was found. It must not be counted as a passed runtime test.
- No flashing, RF test, backend integration test, sensor-data test or concurrency
  measurement has been performed for this migrated firmware yet.

## Test sequence (do not mass erase normal operational boards)

1. Build default in CCS, flash only this TI project, then reset the TI board.
2. Attach UART to Linux backend using 115200 8N1 and the actual serial path.
3. Verify 8A/9A/98 boot frames; issue 0A if boot frames were missed.
4. Open join from UI, boot a test sensor, verify 84/85 and explicit authorization.
5. Submit configuration, distinguish 8E from 90, verify 99 and sensor response.
6. Test temperature/magnetometer/mics, vibration features and V2 chunk/ACK retry.
7. Power-cycle coordinator: PAN/channel persist; join remains closed.
8. Test sleeping-node deletion, wake, reset/rejoin, and persistence read-back.
   A successful coordinator cleanup should produce 8F with statuses 0/0/0 and
   complete=1. Verify remote erase separately using the unchanged node logs.
9. Test malformed UART, bursts, missing acknowledgements and multidevice load.
10. Compare ST/TI channel prepare/commit behavior on test devices only.

## Removal repair, 2026-10-07

Modified project-local Application/gateway_bridge.c, Application/StartUp/main.c,
Stack/zdo/zd_app.c and this document. Added Application/gateway_removal.c/.h.
No ST node sources or SDK installation files were edited; TX remains +5 dBm.
Diagnostics 9F/33 = Leave submission, 34 = cleanup outcome (evidence 1=child
confirm, 2=remote indication), 35 = 30-second wait timeout. Timeout retains the
correlation slot; at most eight outstanding removals are admitted.

CCS full build: zero errors, one existing SysConfig version warning. Actual
gateway_removal.c passed nine host fault-injection scenarios with mocked NV and
RTOS, including read/write/readback errors, missing TC keys and unsuccessful
Leave. Backend main.c compiled for x86_64-linux-musl. Hardware verification and
backend end-to-end runtime tests remain outstanding. Do not mass erase for the
sleeping-node deletion test: retain the current network and its pending tasks.

## Channel update repair, 2026-10-14

- Added ZDO_MGMT_NWKUPDATE_REQUEST to the project-local Stack/Config/f8wcoord.opts.
  ZDO_API_BASIC alone does not enable this request handler; it previously returned
  ZUnsupportedMode (0x12), even when the backend had zero nodes to migrate.
- The bridge refreshes the stack channel/update ID before handling a manual
  request and does not reserve a migration ID after a failed read.
- Kept the existing broadcast plus local ZDO update sequence. Local submission
  errors now return 0x96 with the matching target/phase instead of being dropped.
  Submission success is not channel-change completion: backend 0x8A readback
  remains the source of truth after the stack's broadcast-delivery delay.
- No SDK installation, ST node, backend, frontend, UART speed or RF power changes.
  The modified coordinator sources and option file were already project-local.
- Forced a full default rebuild (not just gateway_bridge.c), because the option
  also enables code in zstacktask.c. Compile/link and HEX generation passed.
- Gateway tests/run-ti-channel-tests.ps1 compiles the actual bridge channel
  functions with stack/UART mocks. Ten scenarios passed, including an empty
  network, broadcast/local/read failures, prepare/commit, current-channel no-op,
  invalid input, cancellation and update-ID/timer wraparound.
- Hardware RF/persistence verification is still required. Flash the updated
  default/zc_cc1352R_coord.out without mass erase; request another manual channel
  with no nodes, confirm actual channel readback, then power-cycle and confirm it
  persists. The backend and ST nodes do not require recompilation for this fix.
