#!/usr/bin/env python3
from pathlib import Path
s=Path('src/rx/frame.cpp').read_text().splitlines()
bal=0
for i,line in enumerate(s, start=1):
    for ch in line:
        if ch=='{': bal+=1
        elif ch=='}': bal-=1
    if bal<0:
        print('negative balance at line', i)
        break
print('final balance', bal)
print('last 10 lines:')
for i in range(len(s)-9, len(s)+1):
    if i<=0 or i>len(s): continue
    print(i, s[i-1])
