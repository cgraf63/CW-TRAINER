import decoder_sim as sim, numpy as np, difflib, sys
REF='CQ CQ DE HB9XYZ HB9XYZ K TNX FER CALL UR RST 599 = NAME KLAUS QTH BERN = 73 TU'
def run(seed):
    sim.rng=np.random.default_rng(seed); x=sim.scene()
    r={}
    for name,f in (('ALT',sim.mic_old),('NEU',sim.mic_new)):
        out=sim.decode(f(x),name=='NEU').split(); ref=REF.split()
        r[name]=(difflib.SequenceMatcher(None,' '.join(out),' '.join(ref)).ratio(), len(out)-len(ref))
    return r
for seed in range(int(sys.argv[1]),int(sys.argv[2])):
    r=run(seed); print(seed, ' '.join(f"{k}: {v[0]*100:.0f}% Treffer, {v[1]:+d} Woerter" for k,v in r.items()), flush=True)
