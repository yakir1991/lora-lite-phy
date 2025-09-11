#!/usr/bin/env python3
from pathlib import Path
import sys

data=Path('src/rx/frame.cpp').read_text()
bal=0
stack=[]
for i,ch in enumerate(data):
    if ch=='{':
        stack.append(i)
        bal+=1
    elif ch=='}':
        if stack:
            stack.pop()
        bal-=1
print('brace balance:', bal)
if bal>0 and stack:
    print('last unmatched open at byte offset:', stack[-1])
sys.exit(0)
