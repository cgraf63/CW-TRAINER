"""Simulation des T-Embed-Mikrofon-Decoders (alte und neue Logik). Start: python3 decoder_bench.py 1 7"""
import numpy as np, sys
FS, N = 16000, 128
MORSE = {'A':'.-','B':'-...','C':'-.-.','D':'-..','E':'.','F':'..-.','G':'--.','H':'....','I':'..','J':'.---','K':'-.-','L':'.-..','M':'--','N':'-.','O':'---','P':'.--.','Q':'--.-','R':'.-.','S':'...','T':'-','U':'..-','V':'...-','W':'.--','X':'-..-','Y':'-.--','Z':'--..',
'0':'-----','1':'.----','2':'..---','3':'...--','4':'....-','5':'.....','6':'-....','7':'--...','8':'---..','9':'----.','/':'-..-.','?':'..--..','=':'-...-'}
REV = {v:k for k,v in MORSE.items()}
rng = np.random.default_rng(1)

def cw(text, wpm, f, amp=0.3):
    d = 1.2/wpm; out=[]
    def tone(t):
        n=int(t*FS); x=amp*np.sin(2*np.pi*f*np.arange(n)/FS)
        r=int(.005*FS); env=np.ones(n); env[:r]=np.linspace(0,1,r); env[-r:]=np.linspace(1,0,r); return x*env
    sil=lambda t: np.zeros(int(t*FS))
    for w in text.split(' '):
        for c in w:
            for j,e in enumerate(MORSE[c]):
                out.append(tone(d if e=='.' else 3*d)); out.append(sil(d))
            out.append(sil(2*d))
        out.append(sil(4*d))
    return np.concatenate(out)

def scene():
    parts=[np.zeros(FS)]
    parts.append(cw('CQ CQ DE HB9XYZ HB9XYZ K', 20, 600))
    parts.append(np.zeros(FS*20))                     # 20 s Pause, nur Rauschen
    parts.append(cw('TNX FER CALL UR RST 599 = NAME KLAUS', 20, 610))
    parts.append(np.zeros(FS*15))
    parts.append(cw('QTH BERN = 73 TU', 25, 700))     # andere Station, schneller, andere Tonhoehe
    x=np.concatenate(parts)
    x=x+rng.normal(0,0.03,len(x))                     # Rauschen
    for _ in range(80):                               # Knackser / Stoerspitzen
        i=rng.integers(0,len(x)-200); x[i:i+rng.integers(20,200)]+=rng.normal(0,0.4,1)
    # gelegentlicher Stoerton (Sprache/Pfeifen) waehrend der Pausen
    i=FS*(1+12); x[i:i+FS//3]+=0.15*np.sin(2*np.pi*850*np.arange(FS//3)/FS)
    return x

def goertzel(x,f):
    k=2*np.cos(2*np.pi*f/FS); s1=s2=0.0
    for v in x: s=v+k*s1-s2; s2=s1; s1=s
    return s1*s1+s2*s2-k*s1*s2

def goertzel_vec(x,freqs):
    n=np.arange(len(x)); out=[]
    for f in freqs:
        c=np.exp(-2j*np.pi*f*n/FS); out.append(abs(np.dot(x,c))**2)
    return np.array(out)

FREQS=400+25*np.arange(25)
WIN=0.5-0.5*np.cos(2*np.pi*np.arange(N)/(N-1))

def mic_old(x):
    ev=[]; binAvg=np.zeros(25); nf,pk,on,lock=0.001,0.01,False,8
    for bi in range(len(x)//N):
        b=x[bi*N:(bi+1)*N]; b=(b-b.mean())*WIN; t=bi*N/FS*1000
        p=goertzel_vec(b,FREQS); binAvg=binAvg*0.99+p*0.01
        best=int(np.argmax(binAvg))
        if binAvg[best]>binAvg[lock]*1.5: lock=best
        mag=np.sqrt(p[lock])/N
        if mag<nf: nf=mag
        else: nf+=(mag-nf)*0.003
        if mag>pk: pk=mag
        else: pk-=(pk-nf)*0.004
        pk=max(pk,nf*1.2); span=pk-nf
        enough=pk>nf*4
        nxt=(enough and mag>nf+span*0.35) if on else (enough and mag>nf+span*0.6)
        if nxt!=on: on=nxt; ev.append((on,t))
    return ev

def mic_new(x):
    ev=[]; binAvg=np.zeros(25); nf,pk,on,lock,cnt=0.001,0.01,False,8,0
    for bi in range(len(x)//N):
        b=x[bi*N:(bi+1)*N]; b=(b-b.mean())*WIN; t=bi*N/FS*1000
        p=goertzel_vec(b,FREQS)
        med=np.median(p)+1e-12; best=int(np.argmax(p))
        tonal=p[best]>med*8                                  # klarer Ton, kein breitbandiges Rauschen
        if tonal: binAvg=binAvg*0.95+p*0.05                  # Tonhoehe nur bei echtem Ton nachfuehren
        cand=int(np.argmax(binAvg))
        if binAvg[cand]>binAvg[lock]*1.3: lock=cand
        lo,hi=max(0,lock-1),min(24,lock+1)
        pl=p[lo:hi+1].max()
        mag=np.sqrt(pl)/N
        if mag<nf: nf=mag
        else: nf+=(mag-nf)*0.002
        if mag>pk: pk=mag
        else: pk-=(pk-nf)*0.0015
        pk=max(pk,nf*1.2); span=pk-nf
        enough=pk>nf*5
        atLock=pl>med*6                                      # der Ton liegt bei der gefundenen Tonhoehe
        want=(enough and mag>nf+span*0.35) if on else (enough and atLock and mag>nf+span*0.6)
        if want!=on:
            cnt+=1
            if cnt>=2: on=want; cnt=0; ev.append((on,t-N/FS*1000))   # 2 Bloecke bestaetigt
        else: cnt=0
    return ev

def decode(ev, new):
    out=''; pat=''; dit=60.0; down=0; up=0; pending=False; spaced=True
    def flush(now):
        nonlocal out,pat,pending,spaced
        gap=now-up
        if pending and gap>2.5*dit: out+=REV.get(pat,'*'); pat=''; pending=False
        if not pending and not spaced and gap>5*dit: out+=' '; spaced=True
    for on,t in ev:
        if on: flush(t); down=t; continue
        d=t-down
        if d<12: continue
        if new and d<0.3*dit: continue                       # Stoerspitze kuerzer als ein Drittel Punkt
        el='.' if d<2*dit else '-'
        est=d if el=='.' else d/3
        if new: est=min(max(est,dit*0.75),dit*1.33); dit=0.85*dit+0.15*est   # begrenzt nachfuehren
        else: dit=0.75*dit+0.25*est
        dit=min(max(dit,25),400 if not new else 300)
        pat+=el; up=t; pending=True; spaced=False
    flush(1e12)
    return out

if __name__=="__main__":
  x=scene()
  for name,f in (('ALT',mic_old),('NEU',mic_new)):
    ev=f(x); print(f'{name}: {len(ev)//2} Elemente erkannt')
    print('  ', decode(ev, name=='NEU'))
