from mamba import description, context, it, fit, before, after
from expects import expect, equal, contain, match
from helper.spec_helper import run_cmd, clean_env, kill_xvnc

with description("Benchmarking"):
    with before.each:
        clean_env()
    with after.each:
        kill_xvnc()
    with it("runs benchmarks"):
        run_cmd("wget --no-check-certificate https://kasmweb-build-artifacts.s3.us-east-1.amazonaws.com/kasmvnc/static/127072-737747495_small.mp4 -O /tmp/video.mp4")
        completed_process = run_cmd("Xvnc -interface 0.0.0.0 :1 -Benchmark /tmp/video.mp4 -VideoArea 100")
        command = '''sed -i "s/KasmVNC/$(grep -E '^ID=' /etc/os-release | cut -d= -f2 | tr -d '"') $(grep -E '^VERSION_CODENAME=' /etc/os-release | cut -d= -f2 | tr -d '"')/g" Benchmark.xml'''
        run_cmd(command)
        expect(completed_process.returncode).to(equal(0))
