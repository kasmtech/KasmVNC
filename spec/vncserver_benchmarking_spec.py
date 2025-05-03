from mamba import description, context, it, fit, before, after
from expects import expect, equal, contain, match
from helper.spec_helper import run_cmd, clean_env, kill_xvnc, clean_locks

with description("Benchmarking"):
    with before.each:
        clean_env()
    with after.each:
        kill_xvnc()
    with it("runs benchmarks"):
        completed_process = run_cmd("Xvnc -interface 0.0.0.0 :1 -selfBench")
        expect(completed_process.returncode).to(equal(0))