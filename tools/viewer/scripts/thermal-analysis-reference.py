#!/usr/bin/env python3
"""Generate independent SciPy/NumPy reference planes; runtime viewer needs neither.

Regenerate with Python + numpy + scipy; versions and all numerical conventions
are recorded in the fixture. No flight data or inferred source labels are used.
"""
import json
from pathlib import Path
import numpy as np
import scipy
from scipy.ndimage import gaussian_filter, grey_opening

w,h=32,24
xx,yy=np.meshgrid(np.arange(w),np.arange(h))
data=np.rint(18000+.4*xx+.3*yy+100*np.exp(-((xx-15.3)**2+(yy-11.7)**2)/2)+np.random.default_rng(120).normal(0,2,(h,w))).astype(np.float32)
sigma=1.0
blur=lambda a:gaussian_filter(a,sigma,mode='reflect',radius=4,output=np.float32)
floor=float(np.quantile(data,.01,method='inverted_cdf'))
observed=np.maximum(data-floor,0)+np.float32(.001)
estimate=observed.copy()
for _ in range(5):
    predicted=blur(estimate)
    residual=observed-predicted
    ratio=np.maximum(.000001,predicted+np.sign(residual)*np.maximum(np.abs(residual)-.75,0))/np.maximum(.000001,predicted)
    estimate=(estimate*blur(ratio)).astype(np.float32)
rl=(estimate+floor-.001).astype(np.float32)
pad=4
pw=2**int(np.ceil(np.log2(w+2*pad)));ph=2**int(np.ceil(np.log2(h+2*pad)))
reflect=lambda i,n: np.where((j:=i%(2*n))<n,j,2*n-1-j)
xidx=reflect(np.arange(pw)-pad,w);yidx=reflect(np.arange(ph)-pad,h)
mean=float(data.astype(np.float64).mean())
def taper(n,start,length):
    result=np.ones(n)
    for i in range(n):
        if i<start:result[i]=np.sin(np.pi/2*i/start)**2
        elif i>=start+length:result[i]=np.sin(np.pi/2*(n-1-i)/(n-1-start-length))**2
    return result
padded=(data[np.ix_(yidx,xidx)].astype(np.float64)-mean)*taper(ph,pad,h)[:,None]*taper(pw,pad,w)[None,:]
kernel=np.exp(-np.arange(-4,5,dtype=float)**2/2);kernel/=kernel.sum()
def transfer(n):return np.cos(2*np.pi*np.arange(n)[:,None]*np.arange(-4,5)[None,:]/n)@kernel
H=transfer(ph)[:,None]*transfer(pw)[None,:]
factor=np.clip(H/(H**2+.02),-8,8)
wiener=(np.fft.ifft2(np.fft.fft2(padded)*factor).real+mean)[pad:pad+h,pad:pad+w].astype(np.float32)
fixture=dict(provenance=dict(numpy=np.__version__,scipy=scipy.__version__,gaussian='half-sample reflect, radius=4, float32',rl='5 iterations, 0.75-count soft residual threshold, P01 floor',wiener='reflect padding 4px, cosine taper outside image, K=.02, gain cap8, double FFT'),width=w,height=h,input=data.astype(np.uint16).ravel().tolist(),gaussian=blur(data).ravel().tolist(),tophat=(data-grey_opening(data,size=(13,13),mode='reflect')).ravel().tolist(),rl=rl.ravel().tolist(),wiener=wiener.ravel().tolist())
output=Path(__file__).resolve().parents[1]/'tests/fixtures/thermal-analysis-reference.json'
output.parent.mkdir(parents=True,exist_ok=True)
output.write_text(json.dumps(fixture,separators=(',',':'))+'\n')
print(output)
