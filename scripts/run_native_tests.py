"""Cross-platform runner for the same production contract suite as CI."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--compiler',nargs='+',default=['g++'])
    parser.add_argument('--sanitizer',default='address,undefined')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='minios-native-') as folder:
        count=0
        for line in (root/'scripts/run_native_tests.sh').read_text().splitlines():
            if not line.startswith('compile_and_run '): continue
            parts=shlex.split(line);name=parts[1];flags=parts[2:]
            flags=[('-fsanitize='+args.sanitizer) if x.startswith('-fsanitize=') else x for x in flags]
            target=Path(folder)/(name+'.exe')
            subprocess.run(args.compiler+['-std=c++11','-Wall','-Wextra','-Werror','-Iinclude']+flags+['-o',str(target)],cwd=root,check=True)
            subprocess.run([str(target)],cwd=root,check=True,timeout=90)
            count+=1
        print(f'{count} native firmware test executables PASS')
if __name__=='__main__':main()
