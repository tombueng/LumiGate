import os,sys,subprocess,tempfile,glob,pcbnew
HERE=os.path.dirname(os.path.abspath(__file__))
BOARD=os.path.join(HERE,"lumigate.kicad_pcb")
# Freerouting 2.x jar + portable JDK 25 live in hardware/tools/ (gitignored).
# Override with env FR2_JAR / FR2_JAVA. FR2 needs Java 25+ (class file v69).
JAR=os.environ.get("FR2_JAR", os.path.join(HERE,"tools","freerouting2.jar"))
JAVA=os.environ.get("FR2_JAVA") or next(iter(glob.glob(os.path.join(HERE,"tools","jdk*","*","bin","java.exe"))), "java")
dsn=os.path.join(tempfile.gettempdir(),"l_fr2.dsn"); ses=os.path.join(tempfile.gettempdir(),"l_fr2.ses")
if os.path.exists(ses): os.remove(ses)
b=pcbnew.LoadBoard(BOARD)
b.SetLayerType(pcbnew.In1_Cu,pcbnew.LT_SIGNAL); b.SetLayerType(pcbnew.In2_Cu,pcbnew.LT_SIGNAL)
n=0; kept=0
for t in list(b.GetTracks()):
    if t.IsLocked(): kept+=1; continue                 # keep locked connector escapes
    b.Remove(t); n+=1
print("cleared",n,"unlocked tracks, kept",kept,"locked",flush=True)
pcbnew.ExportSpecctraDSN(b,dsn)
print("running Freerouting 2.2.4...",flush=True)
r=subprocess.run([JAVA,"-jar",JAR,"-de",dsn,"-do",ses,"-mp","100"],capture_output=True,text=True)
print("FR2 stdout tail:",r.stdout[-300:])
print("FR2 stderr tail:",r.stderr[-300:])
if not os.path.exists(ses): sys.exit("FR2 produced no SES")
pcbnew.ImportSpecctraSES(b,ses)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard(BOARD,b)
print("DONE: saved",flush=True)
