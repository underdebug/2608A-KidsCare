
import sys; sys.path[:0]=["/home/roots/develop2/SLAM/SALM1","/home/roots/.vscode/extensions/local.runpy-0.0.1"]; import importlib, mem; importlib.reload(mem); from mem import *;

n= cpu('n')
d_local = gpu('d_local', 'f', [n, 2])
d_transformed = mem('d_transformed', 'f', [n, 2])
d_map = mem('d_map', 'f', [n, 2])

x1 = d_local[:, 0]
y1 = d_local[:, 1]

x2 = d_transformed[:, 0]
y2 = d_transformed[:, 1]

x3 = d_map[:, 0]
y3 = d_map[:, 1]


fig, axes = plt.subplots(2, 2, figsize=(10, 10))
axes[0][0].plot(x1, y1, 'b.')
axes[0][1].plot(x2, y2, 'r.')
axes[1][0].plot(x3, y3, 'g.')
plt.show()

plt.close('all') 

print('finish')