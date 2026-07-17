#!/usr/bin/env python3
"""
AI Chat 综合测试套件 for ESP32 OscilloGame — 重写版
=======================================================
测试内容:
  1. SSL 稳定性 — 反复调用 DeepSeek，追踪 heap/RTT
  2. 基础重入 — exit→re-enter 循环
  3. Game 跳转模拟 — action 自动退出后重入
  4. 边界情况 — 中途退出、快速启停

设计原则:
  - 每轮前 flush + drain，永不污染跨轮数据
  - 统一 read_until 消费串口，不调串行 wait_for
  - DONE 阶段自动发 btn 退出，统一以 conversation done 收尾
  - FIX 只分析日志，不向设备发命令（防串口淤积）
"""

import serial
import time
import re
import sys
import traceback

PORT = 'COM10'
BAUD = 921600
DEEPSEEK_TIMEOUT = 60

G = "\033[92m"
Y = "\033[93m"
R = "\033[91m"
C = "\033[96m"
N = "\033[0m"


class TestRunner:
    """ESP32 串口测试驱动 — 重写版"""

    def __init__(self, port=PORT, baud=BAUD):
        self.ser = serial.Serial(port, baud, timeout=5)
        self.ser.set_buffer_size(rx_size=65536, tx_size=4096)
        # DTR on open resets ESP32 — drain boot garbage
        self.ser.reset_input_buffer()
        self.log_buf = []
        self.fix_count = 0
        self.max_fixes = 5
        self.stats = {
            'deepseek_200': 0,
            'deepseek_err': 0,
            'rtt_ms': [],
            'heap_start': 0,
            'heap_min': 999999,
            'ssl_reconn': 0,
        }

    # ======================== 底层 IO ========================

    def send(self, cmd):
        """发命令，等 150ms 让固件处理。"""
        self.ser.write((cmd + '\n').encode())
        time.sleep(0.15)

    def _readline(self, timeout=2):
        self.ser.timeout = timeout
        raw = self.ser.readline()
        return raw.decode('utf-8', errors='replace').rstrip('\r\n') if raw else None

    def read_until(self, patterns, timeout=30):
        """持续读直到任一 pattern 匹配。返回 (match, lines) 或 (None, lines)。"""
        compiled = [re.compile(p) if isinstance(p, str) else p for p in patterns]
        deadline = time.time() + timeout
        lines = []
        while time.time() < deadline:
            line = self._readline(0.3)
            if line is None:
                continue
            lines.append(line)
            self.log_buf.append(line)
            for cp in compiled:
                m = cp.search(line)
                if m:
                    return m, lines
        return None, lines

    def wait_for(self, pattern, timeout=15):
        m, _ = self.read_until([pattern], timeout)
        return m is not None

    def drain(self, sec=1.5):
        deadline = time.time() + sec
        while time.time() < deadline:
            self._readline(0.3)

    def flush(self):
        self.ser.reset_input_buffer()

    # ======================== 解析 ========================

    def _parse_line(self, line):
        """从一行日志中提取统计信息。"""
        m = re.search(r'heap at exit:\s*(\d+)', line)
        if m:
            h = int(m.group(1))
            if self.stats['heap_start'] == 0:
                self.stats['heap_start'] = h
            if h < self.stats['heap_min']:
                self.stats['heap_min'] = h
        m = re.search(r'heap:\s*(\d+)', line)
        if m:
            h = int(m.group(1))
            if h < self.stats['heap_min']:
                self.stats['heap_min'] = h
        m = re.search(r'HTTP (\d+)\s*\((\d+)ms\)', line)
        if m:
            code, rtt = int(m.group(1)), int(m.group(2))
            if code == 200:
                self.stats['deepseek_200'] += 1
            else:
                self.stats['deepseek_err'] += 1
            self.stats['rtt_ms'].append(rtt)
        if 'SSL.connected: 0' in line:
            self.stats['ssl_reconn'] += 1

    # ======================== 单轮 ========================

    def _has_reply_in_lines(self, lines):
        """在已消费的行列表中搜索任何 AI 回复迹象."""
        for l in lines:
            if 'Filtered text' in l or 'DeepSeek OK' in l or 'reply OK' in l:
                return True
        return False

    def _extract_reply(self, text):
        """从 Filtered text 行提取 AI 回复内容."""
        m = re.search(r'Filtered text:\s*"(.+)"\s*$', text)
        if m:
            raw = m.group(1)
            # 截断过长回复，保留首 80 字符展示
            return raw[:100] + ('…' if len(raw) > 100 else '')
        return None

    def run_one_round(self, seq, scenario="normal") -> bool:
        """执行一轮 AI Chat 并等待对话结束。

        流程: test:no_mic → test:enter → 等 WAITING → test:btn →
              自动处理 DONE(发btn退)/action(自动退) → conversation done

        成功标准: 检测到 AI 实际回复文本（Filtered text 非空），
                  而非 HTTP 状态码。
        """
        tag = f"[#{seq:02d} {scenario}]"
        print(f"\n  {Y}{tag} Starting...{N}")

        # 清洁串口
        self.flush()
        self.drain(0.5)
        self.send("test:no_mic")
        time.sleep(0.3)
        self.send("test:enter")
        time.sleep(0.3)

        # 等 WAITING（直接等，不分两步——semaphore taken 提前返回会丢 WAITING）
        if not self.wait_for(r'WAITING for ENTER', 20):
            print(f"  {R}{tag} FAIL: never WAITING (20s timeout){N}")
            return False
        print(f"  {G}{tag} WAITING phase{N}")

        # 触发录音
        self.send("test:btn")

        # 循环读，直到 conversation done
        # 核心检测: Filtered text 行提取 AI 回复 → 打印到终端
        # 辅助: DONE 阶段发 btn 退出, action 路径自动等
        got_reply = False
        need_done_btn = False
        while True:
            m, lines = self.read_until([
                r'conversation done',
                r'phase=DONE, polling|DONE, polling ENTER',
                r'exiting for guiTask',
                r'DeepSeek FAILED',
                r'DeepSeek HTTP \d+ \(\d+ms\)',
                r'DEEPSEEK\] Filtered text:',
            ], DEEPSEEK_TIMEOUT + 30)
            if not m:
                print(f"  {R}{tag} FAIL: timeout ({DEEPSEEK_TIMEOUT+30}s){N}")
                return False
            for line in lines:
                self._parse_line(line)

            hit = m.group(0)

            if 'Filtered text' in hit:
                # ★ 抓到 AI 回复文本 → 提取并打印
                raw_line = lines[-1] if lines else ''
                reply_text = self._extract_reply(raw_line) or raw_line
                print(f"  {C}AI: {reply_text}{N}")
                got_reply = True
                continue

            if 'conversation done' in hit:
                # 回扫所有已消费行——Filtered text 可能在 read_until 的同批中
                if not got_reply and self._has_reply_in_lines(lines):
                    # 找到最后一条有回复的行展示
                    for l in reversed(lines):
                        if 'Filtered text' in l:
                            reply_text = self._extract_reply(l) or l
                            print(f"  {C}AI: {reply_text}{N}")
                            got_reply = True
                            break
                if got_reply:
                    print(f"  {G}{tag} conversation done ✓{N}")
                else:
                    print(f"  {Y}{tag} conversation done (no reply){N}")
                break
            if 'DONE' in hit and not need_done_btn:
                print(f"  {Y}{tag} DONE phase, sending btn to exit{N}")
                time.sleep(0.5)
                self.send("test:btn")
                need_done_btn = True
                continue
            if 'exiting' in hit:
                continue  # action 路径，继续等 conversation done
            if 'FAILED' in hit:
                print(f"  {R}{tag} DeepSeek FAILED{N}")
                continue
            if 'HTTP' in hit:
                code = int(re.search(r'HTTP (\d+)', hit).group(1))
                rtt = int(re.search(r'\((\d+)ms\)', hit).group(1))
                status = f"{G}200{N}" if code == 200 else f"{Y}{code}{N}"
                print(f"  {status} {tag} ({rtt}ms)")

        time.sleep(1)
        if got_reply:
            print(f"  {G}{tag} OK{N}")
        else:
            print(f"  {R}{tag} FAIL: no AI reply text{N}")
        return got_reply

    # ======================== 场景 ========================

    def test_ssl_stability(self, rounds=8):
        print(f"\n{C}{'='*60}{N}")
        print(f"{C}  [通路验证] {rounds} 轮 DeepSeek 调用 (以 AI 回复文本为准){N}")
        print(f"{C}{'='*60}{N}")
        passed = 0
        for i in range(1, rounds + 1):
            ok = self.run_one_round(i, "ssl")
            if ok:
                passed += 1
            else:
                self.detect_only()
                if self.fix_count >= self.max_fixes:
                    break
        self._print_ssl_summary(rounds, passed)
        return passed == rounds

    def test_reentry(self, rounds=6):
        print(f"\n{C}{'='*60}{N}")
        print(f"{C}  [退出重入] {rounds} 轮 enter→exit 循环{N}")
        print(f"{C}{'='*60}{N}")
        passed = 0
        for i in range(1, rounds + 1):
            ok = self.run_one_round(i, "reentry")
            if ok:
                passed += 1
            else:
                self.detect_only()
                if self.fix_count >= self.max_fixes:
                    break
        print(f"\n  重入结果: {passed}/{rounds} 通过")
        return passed == rounds

    def test_game_jump(self, rounds=3):
        print(f"\n{C}{'='*60}{N}")
        print(f"{C}  [Game 跳转] {rounds} 轮 action 自动跳转{N}")
        print(f"{C}{'='*60}{N}")
        passed = 0
        for i in range(1, rounds + 1):
            ok = self.run_one_round(i, "game")
            if ok:
                passed += 1
            else:
                self.detect_only()
                if self.fix_count >= self.max_fixes:
                    break
        print(f"\n  Game 跳转结果: {passed}/{rounds} 通过")
        return passed == rounds

    def test_mid_exit(self, rounds=3):
        """中途退出：DeepSeek 未返回时发 exit。"""
        print(f"\n{C}{'='*60}{N}")
        print(f"{C}  [中途退出] {rounds} 轮 DeepSeek 中途 exit{N}")
        print(f"{C}{'='*60}{N}")
        passed = 0
        for i in range(1, rounds + 1):
            tag = f"[#{i:02d} mid-exit]"
            print(f"\n  {Y}{tag} Starting...{N}")
            self.flush()
            self.drain(0.3)
            self.send("test:no_mic")
            time.sleep(0.2)
            self.send("test:enter")
            if not self.wait_for(r'WAITING for ENTER', 12):
                print(f"  {R}{tag} FAIL: never WAITING{N}")
                self.detect_only()
                continue
            self.send("test:btn")
            if not self.wait_for(r'start DeepSeek', DEEPSEEK_TIMEOUT):
                print(f"  {R}{tag} FAIL: never started DeepSeek{N}")
                self.detect_only()
                continue
            time.sleep(1)
            self.send("test:exit")
            if not self.wait_for(r'conversation done', 65):
                print(f"  {R}{tag} FAIL: no exit after mid-exit{N}")
                self.detect_only()
                continue
            time.sleep(1.5)
            print(f"  {G}{tag} OK (mid-exit clean){N}")
            passed += 1
            re_ok = self.run_one_round(i + 100, "mid-reentry")
            if re_ok:
                print(f"  {G}[mid-exit] re-entry OK{N}")
            else:
                print(f"  {R}[mid-exit] re-entry FAIL{N}")
                self.detect_only()
            if self.fix_count >= self.max_fixes:
                break
        print(f"\n  中途退出结果: {passed}/{rounds} 通过")
        return passed >= rounds

    def test_rapid_cycle(self, cycles=5):
        print(f"\n{C}{'='*60}{N}")
        print(f"{C}  [快速启停] {cycles} 次快速 enter→exit{N}")
        print(f"{C}{'='*60}{N}")
        passed = 0
        self.send("test:no_mic")
        time.sleep(0.3)
        for i in range(1, cycles + 1):
            tag = f"[#{i:02d} rapid]"
            print(f"\n  {Y}{tag} enter...{N}")
            self.flush()
            self.drain(0.2)
            self.send("test:enter")
            time.sleep(2.0)
            self.send("test:exit")
            if self.wait_for(r'conversation done', 6):
                passed += 1
                print(f"  {G}{tag} OK{N}")
            else:
                print(f"  {Y}{tag} no exit (maybe fine){N}")
            time.sleep(0.5)
        print(f"\n  快速启停结果: {passed}/{cycles} 干净退出")
        print(f"  {C}[rapid] final re-entry...{N}")
        ok = self.run_one_round(999, "rapid-final")
        if ok:
            print(f"  {G}rapid-cycle re-entry OK{N}")
            return True
        self.detect_only()
        return False

    # ======================== 诊断 ========================

    def detect_only(self):
        """只分析日志，不向设备发命令。"""
        self.fix_count += 1
        if self.fix_count > self.max_fixes:
            return
        print(f"\n  {Y}[DIAG #{self.fix_count}] 分析失败原因...{N}")
        log = '\n'.join(self.log_buf[-200:])
        if re.search(r'FAILED to create task', log):
            print(f"  {R}task 创建失败 (堆不足){N}")
        elif re.search(r'connection error', log):
            print(f"  {R}DeepSeek SSL 连接错误{N}")
        elif re.search(r'heap.*?\d{2,4}$', log):
            print(f"  {R}堆严重不足{N}")
        elif re.search(r'JSON parse fail', log):
            print(f"  {R}DeepSeek JSON 解析失败（body 截断）{N}")
        elif re.search(r'empty response', log):
            print(f"  {R}DeepSeek 空响应{N}")
        else:
            print(f"  {Y}最后 20 行:\n" + '\n'.join(self.log_buf[-20:]) + f"{N}")

    # ======================== 报告 ========================

    def _print_ssl_summary(self, rounds, passed):
        s = self.stats
        print(f"\n  通路结果: {passed}/{rounds} 通过 (有 AI 回复)")
        if s['heap_start']:
            leak = s['heap_start'] - s['heap_min']
            print(f"  堆: start={s['heap_start']}, min={s['heap_min']}, "
                  f"{'泄漏 ' + str(leak) + ' bytes' if leak > 5000 else 'OK'}")
        print(f"  SSL 重连: {s['ssl_reconn']} 次")
        if s['rtt_ms']:
            avg = sum(s['rtt_ms']) / len(s['rtt_ms'])
            print(f"  RTT: avg={avg:.0f}ms, max={max(s['rtt_ms'])}ms")

    def print_report(self):
        s = self.stats
        total = s['deepseek_200'] + s['deepseek_err']
        rate = (s['deepseek_200'] / total * 100) if total else 0
        print(f"\n{C}{'='*60}{N}")
        print(f"{C}  最终测试报告{N}")
        print(f"{C}  (通路标准: AI 实际回复文本){N}")
        print(f"{C}{'='*60}{N}")
        print(f"  DeepSeek HTTP: {total} 次, 200={s['deepseek_200']} ({rate:.1f}%), err={s['deepseek_err']}")
        print(f"  SSL 重连: {s['ssl_reconn']} 次")
        if s['rtt_ms']:
            avg = sum(s['rtt_ms']) / len(s['rtt_ms'])
            print(f"  RTT: avg={avg:.0f}ms, max={max(s['rtt_ms'])}ms")
        if s['heap_start']:
            leak = s['heap_start'] - s['heap_min']
            print(f"  堆: start={s['heap_start']}, min={s['heap_min']}, "
                  f"{'泄漏 ' + str(leak) + ' bytes' if leak > 5000 else 'OK'}")
        print(f"  修复: {self.fix_count}/{self.max_fixes}")
        print(f"{C}{'='*60}{N}\n")


def main():
    quick = '--quick' in sys.argv
    stress = None
    for a in sys.argv:
        if a.startswith('--stress='):
            stress = int(a.split('=')[1])

    runner = TestRunner(PORT)
    print(f"{C}{'='*60}{N}")
    print(f"{C}  ESP32 AI Chat 综合测试 (AI 回复通路版){N}")
    print(f"{C}  串口: {PORT} @ {BAUD} baud{N}")
    print(f"{C}  判定标准: AI 回复文本 (非 HTTP 状态码){N}")
    print(f"{C}{'='*60}{N}")

    print(f"\n{Y}等待 ESP32 就绪 (DTR 复位后需 5s+)...{N}")
    runner.drain(6)
    # 验证 ESP32 在线: 发 no_mic 等响应
    runner.send("test:no_mic")
    if not runner.wait_for(r'TEST.*set test_no_mic', 3):
        print(f"{R}ESP32 无响应 — 串口不通或仍在复位{N}")
        runner.drain(3)
        runner.send("test:no_mic")
        if not runner.wait_for(r'TEST.*set test_no_mic', 3):
            print(f"{R}ESP32 确实无响应，退出{N}")
            sys.exit(1)
    runner.flush()
    print(f"{G}ESP32 就绪 ✓{N}")

    results = []
    ssl_r = stress or (3 if quick else 8)
    re_r = stress or (3 if quick else 6)
    game_r = max(1, re_r // 2)
    mid_r = max(1, re_r // 2)
    rapid_n = 3 if quick else 5

    for name, fn, n in [
        ("SSL 稳定性", runner.test_ssl_stability, ssl_r),
        ("退出重入", runner.test_reentry, re_r),
        ("Game 跳转", runner.test_game_jump, game_r),
        ("中途退出", runner.test_mid_exit, mid_r),
        ("快速启停", runner.test_rapid_cycle, rapid_n),
    ]:
        print(f"\n{C}>>> {name} ({n}) <<<{N}")
        ok = fn(n)
        results.append((name, ok))
        if not ok and runner.fix_count >= runner.max_fixes:
            print(f"  {R}max fixes reached, aborting{N}")
            break

    runner.print_report()
    for name, ok in results:
        print(f"  [{G}PASS{N if ok else R+'FAIL'+N}] {name}")
    print(f"\n{'='*60}")
    print(f"{G if all(ok for _, ok in results) else R}  {'全部通过!' if all(ok for _, ok in results) else '存在失败'}{N}")
    print(f"{'='*60}")
    return 0 if all(ok for _, ok in results) else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print(f"\n{Y}用户中断{N}")
        sys.exit(130)
    except Exception as e:
        print(f"\n{R}脚本异常: {e}{N}")
        traceback.print_exc()
        sys.exit(2)
