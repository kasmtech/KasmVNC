"""Check the portable runtime harness's actual software-frame assertion."""
import ast
from pathlib import Path
import re
import unittest


SOURCE = Path(__file__).resolve().parents[2] / 'builder/test-portable'
FAILURE = 'Server did not confirm software H.264 frame encoding'
TREE = ast.parse(SOURCE.read_text())
ASSERTION = next(node for node in ast.walk(TREE)
                 if isinstance(node, ast.If) and len(node.body) == 1
                 and isinstance(node.body[0], ast.Raise)
                 and any(isinstance(child, ast.Constant) and child.value == FAILURE
                         for child in ast.walk(node.body[0])))
CHECK = compile(ast.Module(body=[ASSERTION], type_ignores=[]), str(SOURCE), 'exec')


class PortableEncodingLog(unittest.TestCase):
    def check(self, logs):
        exec(CHECK, {'logs': logs, 're': re})

    def test_accepts_encoded_software_h264_frames_in_both_log_formats(self):
        # Current line is from the failed Alpine 3.21 runtime job's server log.
        for logger in ('SoftwareEncoder', 'FFmpegVideoEncoder: Software libx264'):
            with self.subTest(logger=logger):
                self.check(' 2026-11-13 15:58:20,737 [DEBUG] ' + logger
                           + ': Screen id 0, codec 16, frame size: 3116\n')

    def test_rejects_non_software_h264_and_unencoded_logs(self):
        messages = [
            'FFmpegVideoEncoder: Hardware h264_nvenc: Screen id 0, codec 16, frame size: 3116',
            'FFmpegVideoEncoder: Software libx265: Screen id 0, codec 32, frame size: 3116',
            'FFmpegVideoEncoder: Software libx264: Screen id 1, codec 16, frame size: 3116',
            'FFmpegVideoEncoder: Software libx264: Screen id 0, codec 32, frame size: 3116',
            'FFmpegVideoEncoder: Software libx264: Constructor: device=',
            'EncoderProbe: Available encoders: libx264 libx265',
        ]
        for logger in ('SoftwareEncoder', 'FFmpegVideoEncoder: Software libx264'):
            for size in ('0', '-1', ''):
                messages.append(logger + ': Screen id 0, codec 16, frame size: ' + size)
        for message in messages:
            with self.subTest(message=message), self.assertRaisesRegex(RuntimeError, FAILURE):
                self.check(message)


if __name__ == '__main__':
    unittest.main()
