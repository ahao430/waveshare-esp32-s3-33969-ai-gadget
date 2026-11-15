package main
import ("context";"fmt";"os";"gadget-bridge/internal/security";"gadget-bridge/internal/storage";"gadget-bridge/internal/voice")
func main(){
 mk,err:=security.ParseMasterKey(os.Getenv("BRIDGE_MASTER_KEY")); if err!=nil{panic(err)}
 db,_:=storage.Open("data/gadget.db")
 label,key,err:=db.GetCredential("zhipu",mk)
 fmt.Printf("label=%q key_len=%d err=%v\n",label,len(key),err)
 if key=="" {return}
 pcm,err:=os.ReadFile("/tmp/voiceprobe/asr_test.pcm"); if err!=nil{panic(err)}
 txt,err:=voice.ZhipuTranscribeProbe(context.Background(),key,"",pcm)
 fmt.Printf("text=%q err=%v\n",txt,err)
}
