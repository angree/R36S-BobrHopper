import sys, collections
sys.argv=['x']; exec(open('tools/planar_measure.py').read().split("for p in sys.argv")[0])
ents,_=load('data_amiga/sprites.spr')
fw=collections.defaultdict(list)
for i,(w,h,px) in enumerate(ents):
    f=family(names.get(i,'?'))
    rows=[r for r in range(h) if any(px[r*w:(r+1)*w])]; cols=[c for c in range(w) if any(px[r*w+c] for r in range(h))]
    if not rows: continue
    bw=cols[-1]-cols[0]+1; bh=rows[-1]-rows[0]+1
    fw[f].append(((bw+15)//16+1)*bh)
avg={f:sum(v)/len(v) for f,v in fw.items()}
TICK=1/7.09379e6
cook=8*TICK; rest=4*TICK
scen={
 'spokojna (1 kloda, 2 auta, bohater, 1 lilia)':{'LOG_2':1,'ORANGE_CAR':1,'RED_TRUCK':1,'BEAVER':1,'LILY_PAD':1},
 'typowa (3 klody, 4 auta, bohater, 2 lilie, 2 drzewa zaslaniajace)':{'LOG_1':1,'LOG_2':1,'LOG_3':1,'ORANGE_CAR':2,'RED_TRUCK':1,'BLUE_CAR':1,'BEAVER':1,'LILY_PAD':2,'TREE_2':2},
 'ciezka (pociag 1+3+1, 4 klody, 6 aut, bohater)':{'TRAIN_FRONT':1,'TRAIN_MIDDLE':3,'TRAIN_BACK':1,'LOG_1':2,'LOG_3':2,'ORANGE_CAR':3,'RED_TRUCK':2,'BLUE_CAR':1,'BEAVER':1,'TREE_2':2},
}
print("slowa na plan (srednio): "+", ".join("%s %.0f"%(k,avg[k]) for k in ['BEAVER','ORANGE_CAR','RED_TRUCK','LOG_3','TRAIN_MIDDLE','TREE_2']))
for name,s in scen.items():
    words=sum(avg[f]*n for f,n in s.items())
    line=[]
    for planes in (8,6,5,4):
        t=words*planes*(cook+rest)*1000
        line.append("%dpl %.0f ms"%(planes,t))
    print("%-62s slow/plan %5.0f | %s"%(name,words," | ".join(line)))
