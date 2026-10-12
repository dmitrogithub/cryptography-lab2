"""Exercise the actual console application, including UTF-8 and step controls.

Usage: python tests/test_cli.py [path/to/kupyna-lab.exe]
All generated files stay under this project's ignored build/cli-checks folder.
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EXE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "bin" / "kupyna-lab.exe"
WORK = ROOT / "build" / "cli-checks"
WORK.mkdir(parents=True, exist_ok=True)


def run(*args, stdin=None, code=0):
    result = subprocess.run(
        [str(EXE), *args], input=stdin, text=True, encoding="utf-8",
        capture_output=True, cwd=WORK, timeout=30,
    )
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout + result.stderr


def digest(output, bits):
    matches = re.findall(rf"(?<![A-F0-9])[A-F0-9]{{{bits // 4}}}(?![A-F0-9])", output)
    assert matches, output
    return matches[-1]


def check(name, test):
    test()
    print("[OK]", name)


def empty_vectors():
    assert digest(run("hash", "--bits", "256", "--hex", ""), 256) == (
        "CD5101D1CCDF0D1D1F4ADA56E888CD724CA1A0838A3521E7131D4FB78D0F5EB6"
    )
    assert digest(run("hash", "--bits", "512", "--hex", ""), 512) == (
        "656B2F4CD71462388B64A37043EA55DBE445D452AECD46C3298343314EF04019"
        "BCFA3F04265A9857F91BE91FCE197096187CEDA78C9C1C021C294A0689198538"
    )


def encodings_and_file():
    text = "Купина: привіт, Україно! 🔐"
    source = WORK / "повідомлення.bin"
    source.write_bytes(text.encode("utf-8"))
    for bits in (256, 512):
        text_hash = digest(run("hash", "--bits", str(bits), "--text", text), bits)
        assert digest(run("hash", "--bits", str(bits), "--hex", text.encode().hex()), bits) == text_hash
        assert digest(run("hash", "--bits", str(bits), "--file", str(source)), bits) == text_hash


def partial_bits():
    for bits in (256, 512):
        assert digest(run("hash", "--bits", str(bits), "--hex", "A0", "--message-bits", "3"), bits) == (
            digest(run("hash", "--bits", str(bits), "--hex", "BF FF", "--message-bits", "3"), bits)
        )


def saved_trace():
    for bits, count in ((256, 130), (512, 178)):
        destination = WORK / f"кроки-{bits}.txt"
        summary = run("trace", "--bits", str(bits), "--text", "abc", "--matrix", "--output", str(destination))
        trace = destination.read_text(encoding="utf-8")
        assert len(re.findall(r"\[Крок \d+\]", trace)) == count
        for operation in ("Padding", "Initialize", "XorInput", "AddRoundConstant", "SubBytes", "ShiftRows",
                          "MixColumns", "Compression", "OutputXor", "Truncate"):
            assert operation in trace
        assert "s[row, col]" in trace
        assert "round=0" in trace and f"round={9 if bits == 256 else 13}" in trace
        assert digest(trace, bits) == digest(summary, bits) == digest(run("hash", "--bits", str(bits), "--text", "abc"), bits)


def menu_and_pause():
    # Interactive text default, display all HEX steps, no file, exit.
    output = run(stdin="1\n1\n\n0\n0\n\n0\n")
    assert "[Крок 130] Truncate" in output
    assert digest(output, 256) == digest(run("hash", "--bits", "256", "--text", "abc"), 256)
    # Each paused callback consumes input before the following operation runs.
    output = run("trace", "--text", "abc", "--pause", stdin="\nq\n")
    assert "[Крок 1]" in output and "[Крок 2]" in output and "[Крок 3]" not in output
    output = run("trace", "--text", "abc", "--pause", stdin="a\n")
    assert "[Крок 130] Truncate" in output
    output = run(stdin="3\n3\n0\n")
    assert "Результат Купина-256" in output and "Результат Купина-512" in output


def reject_invalid():
    cases = [
        ("hash", "--hex", "G0"), ("hash", "--hex", "0"),
        ("hash", "--text", "abc", "--bits", "384"),
        ("hash", "--hex", "FF", "--message-bits", "9"),
        ("hash", "--text", "abc", "--message-bits", "-1"),
        ("hash", "--text", "abc", "--hex", "00"),
        ("hash", "--file", str(WORK / "missing.bin")), ("hash",),
        ("trace", "--text", "x" * 1025), ("hash", "--hex", "00", "--matrix"),
        ("trace", "--text", "a", "--unknown"), ("trace", "--text"),
    ]
    for args in cases:
        assert "Помилка:" in run(*args, code=2)


def preserve_input_and_rejected_outputs():
    source = WORK / "original.bin"
    content = b"keep original input"
    source.write_bytes(content)
    run("trace", "--file", str(source), "--output", str(source), code=2)
    assert source.read_bytes() == content
    trace = WORK / "existing.txt"
    trace.write_bytes(content)
    run("trace", "--text", "x" * 1025, "--output", str(trace), code=2)
    assert trace.read_bytes() == content
    # The interactive branch validates before opening an existing destination too.
    output = run(stdin=f"1\n2\nFF\n9\n0\n0\n{trace}\n0\n")
    assert "Помилка:" in output and trace.read_bytes() == content


checks = [
    ("empty standard vectors through CLI", empty_vectors),
    ("UTF-8 text, HEX and Unicode file path agree", encodings_and_file),
    ("partial-bit CLI ignores unused bits", partial_bits),
    ("saved traces contain all real stages and correct digest", saved_trace),
    ("interactive menu and per-step pause / quit / continue", menu_and_pause),
    ("invalid arguments and limits fail clearly", reject_invalid),
    ("source file and rejected output files are preserved", preserve_input_and_rejected_outputs),
]
for name, test in checks:
    check(name, test)
print(f"CLI checks: {len(checks)}/{len(checks)} passed.")
