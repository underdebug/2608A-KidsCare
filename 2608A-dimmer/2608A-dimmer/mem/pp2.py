
dst = mem('dst'), print(dst)

a = dst['x']
print(a)


dst = mem('dst')

print(size(dst))
print(type(dst))

print(dst)

bestIdx = mem('bestIdx', None, None, 1)


bestIdx = mem('query', None, 10, 1)
print(bestIdx)

bestIdx = mem('target', None, 10, 1)
print(bestIdx)

bestIdx = mem('bestDist2', None, 10, 1)
print(bestIdx)

para = gdb.parse_and_eval('bestDist2')

para = gdb.parse_and_eval('bestIdx')