#!/usr/bin/env python3
"""Simulate the FSE spread algorithm used in build_seq_fse."""

def spread(norm, tableLog):
    tableSize = 1 << tableLog
    high = tableSize - 1
    step = (tableSize >> 1) + (tableSize >> 3) + 3
    # place low-prob symbols
    for s in norm:
        pass
    n_low = sum(1 for x in norm if x == -1)
    high -= n_low
    pos = 0
    placed = 0
    for s, c in enumerate(norm):
        for _ in range(c):
            placed += 1
            pos = (pos + step) & (tableSize - 1)
            while pos > high:
                pos = (pos + step) & (tableSize - 1)
    return pos, placed, step, sum(abs(x) for x in norm)

LL = [4,3,2,2,2,2,2,2,2,2,2,2,2,1,1,1,2,2,2,2,2,2,2,2,2,3,2,1,1,1,1,1,-1,-1,-1,-1]
ML = [1,4,3,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1]
OF = [1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1]

print("LL log6:", spread(LL, 6))
print("ML log6:", spread(ML, 6))
print("OF log5:", spread(OF, 5))
