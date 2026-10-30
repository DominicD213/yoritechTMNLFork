'use strict';
// Public dummy credentials and an independent Node crypto implementation check
// the actual C++ endpoint/storage. Optional local cross-repo mode imports the desktop.
const assert=require('node:assert/strict'),{spawn}=require('node:child_process'),{createInterface}=require('node:readline');
const {randomBytes,createHash,hkdfSync,createCipheriv,createDecipheriv}=require('node:crypto');
const path=require('node:path');
const protocol='avolyn-presets-lan-v1';
const record={ip:'192.168.1.44',macAddress:'68:ee:8f:50:5b:50',keyId:'101112131415161718191a1b1c1d1e1f',secret:'000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f'};
const id=()=>randomBytes(16).toString('hex');
function crypt(context,direction,challenge,value,requestIv){
  const key=Buffer.from(hkdfSync('sha256',Buffer.from(record.secret,'hex'),Buffer.from(record.keyId),Buffer.from(`${protocol}/${context}/${direction}`),32));
  const aad=Buffer.from(`${protocol}\n${direction}\n${record.keyId}\n${challenge}\n${context}${direction==='response'?'\n'+requestIv:''}`);
  if(direction==='request'){
    const iv=randomBytes(12),cipher=createCipheriv('aes-256-gcm',key,iv);cipher.setAAD(aad);
    return {v:1,keyId:record.keyId,contextId:context,iv:iv.toString('hex'),ciphertext:Buffer.concat([cipher.update(JSON.stringify(value)),cipher.final()]).toString('hex'),tag:cipher.getAuthTag().toString('hex')};
  }
  const cipher=createDecipheriv('aes-256-gcm',key,Buffer.from(value.iv,'hex'));cipher.setAAD(aad);cipher.setAuthTag(Buffer.from(value.tag,'hex'));
  return JSON.parse(Buffer.concat([cipher.update(Buffer.from(value.ciphertext,'hex')),cipher.final()]).toString());
}
async function main(){
  const child=spawn(process.argv[2],[process.argv[3]],{stdio:['pipe','pipe','inherit']});
  const lines=createInterface({input:child.stdout});let pending;let now=0;
  lines.on('line',line=>{const p=pending;pending=null;if(p)p.resolve(JSON.parse(line));});
  child.on('exit',code=>{if(pending)pending.reject(Error('C++ endpoint exited: '+code));});
  const host=input=>new Promise((resolve,reject)=>{assert.ok(!pending);pending={resolve,reject};child.stdin.write(JSON.stringify({now:now+=200,...input})+'\n');});
  let lastChallenge,drop=null,largestResponse=0;const seen=new Set();
  // Decode test requests with the request-direction key, independent of production code.
  function readRequest(envelope,challenge){
    const context=envelope.contextId,key=Buffer.from(hkdfSync('sha256',Buffer.from(record.secret,'hex'),Buffer.from(record.keyId),Buffer.from(`${protocol}/${context}/request`),32));
    const cipher=createDecipheriv('aes-256-gcm',key,Buffer.from(envelope.iv,'hex'));cipher.setAAD(Buffer.from(`${protocol}\nrequest\n${record.keyId}\n${challenge}\n${context}`));cipher.setAuthTag(Buffer.from(envelope.tag,'hex'));
    return JSON.parse(Buffer.concat([cipher.update(Buffer.from(envelope.ciphertext,'hex')),cipher.final()]).toString());
  }
  const fetchWire=async(url,options={})=>{
    if(url.endsWith('/preset-challenge')){lastChallenge=await host({action:'challenge',random:randomBytes(32).toString('hex')});return new Response(JSON.stringify(lastChallenge));}
    assert.ok(url.endsWith('/presets'));assert.ok(Buffer.byteLength(options.body)<=4608);
    const envelope=JSON.parse(options.body),message=readRequest(envelope,lastChallenge.challenge);
    const uniqueness=envelope.contextId+envelope.iv;assert.ok(!seen.has(uniqueness));seen.add(uniqueness);
    const response=await host({action:'request',envelope,iv:randomBytes(12).toString('hex')});
    largestResponse=Math.max(largestResponse,Buffer.byteLength(JSON.stringify(response.body)));
    if(drop&&drop(message)){drop=null;throw new TypeError('Simulated lost acknowledgement');}
    return new Response(JSON.stringify(response.body),{status:response.status});
  };
  let request;
  const desktop=process.env.AVOLYN_DESKTOP_ROOT;
  if(desktop){const {PresetLanClient}=require(path.join(desktop,'electron/preset-lan-client.cjs'));const client=new PresetLanClient(record,{fetchImpl:fetchWire,wait:async()=>{}});request=message=>client.request(message);}
  else request=async message=>{
    const context=message.transferId||id();let uncertain=false;
    for(let n=0;n<3;n++)try{
      const challenge=await(await fetchWire('http://test/preset-challenge')).json();
      const envelope=crypt(context,'request',challenge.challenge,{...message,challenge:challenge.challenge});
      const response=await fetchWire('http://test/presets',{body:JSON.stringify(envelope)});assert.equal(response.status,200);
      const reply=crypt(context,'response',challenge.challenge,await response.json(),envelope.iv);
      assert.equal(reply.lanProtocol,protocol);assert.equal(reply.requestId,message.requestId);
      if(!reply.ok){const e=Error(reply.error);e.deviceCode=reply.error;e.uncertain=uncertain;throw e;}return reply;
    }catch(e){if(!(e instanceof TypeError)||n===2)throw e;uncertain=true;}
  };
  const command=(cmd,data={})=>request({cmd,requestId:id(),...data});
  const state=()=>host({action:'state'});
  const raw=async(cmd,context=id(),extra={})=>{
    const challenge=await host({action:'challenge',random:randomBytes(32).toString('hex')});
    const envelope=crypt(context,'request',challenge.challenge,{cmd,requestId:id(),challenge:challenge.challenge,...extra});
    const response=await host({action:'request',envelope,iv:randomBytes(12).toString('hex')});
    return {challenge,envelope,response,context};
  };
  try{
    const vector=require('./preset-lan-v1.json');
    await host({action:'challenge',random:vector.challenge});
    const vectorReply=await host({action:'request',envelope:vector.request,iv:vector.response.iv});
    assert.equal(vectorReply.status,200);assert.deepEqual(vectorReply.body,vector.response);
    assert.equal((await host({action:'request',envelope:{cmd:'PRESET_DELETE'},iv:'00'.repeat(12)})).status,401);
    const capture=await raw('FRAME_CAPS');assert.equal(capture.response.status,200);
    assert.equal((await host({action:'request',envelope:capture.envelope,iv:'00'.repeat(12)})).status,401);
    for(const mutate of [e=>({...e,tag:'00'.repeat(16)}),e=>({...e,contextId:id()}),e=>({...e,ciphertext:e.ciphertext+'00'.repeat(2048)})]){
      const ch=await host({action:'challenge',random:randomBytes(32).toString('hex')}),context=id();
      const env=crypt(context,'request',ch.challenge,{cmd:'PRESET_LIST',requestId:id(),challenge:ch.challenge});
      assert.equal((await host({action:'request',envelope:mutate(env),iv:'00'.repeat(12)})).status,401);
    }
    const invalid=await raw('PAIR_SET');assert.equal(crypt(invalid.context,'response',invalid.challenge.challenge,invalid.response.body,invalid.envelope.iv).error,'unsupported_command');
    const cross=await raw('FRAME_BEGIN',id(),{transferId:id()});assert.equal(crypt(cross.context,'response',cross.challenge.challenge,cross.response.body,cross.envelope.iv).error,'invalid_transfer_scope');
    const caps=await command('FRAME_CAPS');assert.equal(caps.storageSupported,true);assert.equal(caps.usbReceiveReady,false);
    const images=[0x35,0xa6,0x7f].map(byte=>Buffer.alloc(26928,byte));
    async function upload(slot,pixels,expectedSlot,name='"\\'.repeat(32)){
      const input={profile:'waveshare-579-bw',width:792,height:272,pixelFormat:'mono1-msb-white1',pixels,presetSlot:slot,presetId:'preset-'+slot,presetName:name,expectedSlot};
      if(desktop){const {validateFrame,transferFrame}=require(path.join(desktop,'electron/frame-transfer.cjs'));return transferFrame(validateFrame(input),request,()=>{},{transport:'wifi'});}
      const transferId=id(),sha256=createHash('sha256').update(pixels).digest('hex');
      await command('FRAME_BEGIN',{transferId,profile:input.profile,width:792,height:272,stride:99,pixelFormat:input.pixelFormat,bytes:26928,sha256,slot,presetProtocol:'avolyn-presets-v1',presetId:createHash('sha256').update('avolyn-preset-id-v1\0'+input.presetId).digest('hex').slice(0,32),presetName:name,expectedSlot});
      try{for(let offset=0;offset<pixels.length;offset+=512){const chunk=pixels.subarray(offset,offset+512);assert.equal((await command('FRAME_CHUNK',{transferId,offset,data:chunk.toString('base64')})).nextOffset,offset+chunk.length);}
        const result=await command('FRAME_COMMIT',{transferId});assert.equal(result.persistent,true);return result;
      }catch(e){await command('FRAME_CANCEL',{transferId});throw e;}
    }
    for(let slot=1;slot<=3;slot++){
      drop=message=>slot===1?message.cmd==='FRAME_CHUNK'&&message.offset===1024:slot===2?message.cmd==='FRAME_COMMIT':false;
      await upload(slot,images[slot-1],{occupied:false});drop=null;
    }
    assert.equal((await state()).writes,3);
    assert.ok(largestResponse<=4608);await host({action:'reboot'});
    let list=await command('PRESET_LIST');assert.equal(list.slots.filter(s=>s.valid).length,3);assert.equal(list.activePresetSlot,3);
    await assert.rejects(command('PRESET_DELETE',{slot:1,frameId:id(),sha256:list.slots[0].sha256}));
    await command('PRESET_SELECT',{slot:1,sha256:list.slots[0].sha256});await host({action:'reboot'});assert.equal((await state()).selectedPresetSlot,1);
    for(const fault of ['sync','rename']){
      list=await command('PRESET_LIST');await host({action:'fault',fault});
      await assert.rejects(upload(1,Buffer.alloc(26928,0x22),{occupied:true,frameId:list.slots[0].frameId,sha256:list.slots[0].sha256}));
      await host({action:'fault',fault:''});assert.equal((await state()).slots[0].sha256,list.slots[0].sha256);
    }
    list=await command('PRESET_LIST');
    await assert.rejects(upload(1,images[0],{occupied:false}));
    const transferId=id();await command('FRAME_BEGIN',{transferId,profile:'waveshare-579-bw',width:792,height:272,stride:99,pixelFormat:'mono1-msb-white1',bytes:26928,sha256:createHash('sha256').update(images[0]).digest('hex'),slot:1,presetProtocol:'avolyn-presets-v1',presetId:id(),presetName:'Interrupted',expectedSlot:{occupied:true,frameId:list.slots[0].frameId,sha256:list.slots[0].sha256}});
    assert.equal((await host({action:'usb',command:{cmd:'PRESET_SELECT',requestId:id(),slot:2,sha256:list.slots[1].sha256}})).error,'busy');
    await command('FRAME_CHUNK',{transferId,offset:0,data:images[0].subarray(0,512).toString('base64')});await host({action:'reboot'});
    list=await command('PRESET_LIST');assert.equal(list.slots[0].sha256,createHash('sha256').update(images[0]).digest('hex'));
    const before=await raw('PRESET_LIST');await host({action:'reset-key'});assert.equal((await host({action:'request',envelope:before.envelope,iv:'00'.repeat(12)})).status,401);
    await command('PRESET_DELETE',{slot:1,frameId:list.slots[0].frameId,sha256:list.slots[0].sha256});await host({action:'reboot'});
    const final=await command('PRESET_LIST');assert.equal(final.slots[0].occupied,false);assert.equal(final.slots[1].sha256,list.slots[1].sha256);assert.equal(final.slots[2].sha256,list.slots[2].sha256);assert.equal(final.activePresetSlot,0);
    console.log('Authenticated C++/Node preset interoperability, three-slot reboot persistence, lost ACKs, tampering/replay, stale writes, USB contention and atomic failure recovery passed'+(desktop?' using actual desktop modules.':'.'));
  }finally{child.stdin.end();await new Promise((resolve,reject)=>{const done=code=>code===0?resolve():reject(Error('C++ sanitizer bridge failed: '+code));if(child.exitCode!==null)done(child.exitCode);else child.once('exit',done);});}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
