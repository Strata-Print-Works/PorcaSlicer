// Socket-free execution of the shipped vendor MQTT handshake, not a parallel reimplementation.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../resources/web/elegoolink/lan_service_web/index.html'), 'utf8');
const start = html.indexOf('function lbe(e)');
const end = html.indexOf('let mA=N0;', start);
assert(start >= 0 && end > start, 'vendor handshake boundaries changed; review the fixture');
function fixture(id) {
  const timers = new Map(); let nextTimer = 0;
  const events = []; const published = [];
  const client = {connected:true, handlers:{}, on(event, fn){this.handlers[event]=fn;return this;},
    subscribe(topic, opts, fn){fn(null);}, publish(topic, body, opts, fn){published.push({topic,body:JSON.parse(body)});fn(null);},
    end(){this.connected=false;}, reconnect(){events.push('reconnect');}};
  let connection;
  const context = vm.createContext({Map, Set, Date, JSON, Promise, Error,
    et:(o,k,v)=>o[k]=v, uT:()=>({emit:event=>events.push(event)}), we:{log(){},warn(){},error(){}},
    Ni:{ip:'192.0.2.10',username:'elegoo',access_code:'fixture-code'},
    abe:{connect:(url,options)=>{connection={url,options};return client;}},
    tc:{global:{t:k=>k}}, ib:{TooManyClients:'too many clients'},
    Pt:{MessageAutoReport:'status',SystemAutoReport:'system',GetBasicInfo:'base'}, tb:x=>x,
    setTimeout:fn=>{timers.set(++nextTimer,fn);return nextTimer;},clearTimeout:id=>timers.delete(id),
    setInterval:fn=>{timers.set(++nextTimer,fn);return nextTimer;},clearInterval:id=>timers.delete(id)});
  vm.runInContext(html.slice(start,end)+';globalThis.subject=N0;',context);
  const subject = new context.subject(); subject.setClientId(id); subject.setSn('fixture-sn'); subject.setRegisterRequestId('request-1');subject.connect();
  return {subject,client,timers,events,published,get connection(){return connection;}};
}
async function register(f, reply, topic='elegoo/fixture-sn/request-1/register_response') {
  const result = f.subject.requestRegister('elegoo/fixture-sn/api_register', topic, {request_id:'request-1'});
  await new Promise(setImmediate);
  f.subject.onReceiveMessage(topic,JSON.stringify(reply));
  return result;
}
let completed=false;
process.on("beforeExit",()=>{if(!completed){console.error("FAIL: fixture ended with unresolved handshake");process.exitCode=1;}});
(async()=>{
  const a=fixture('client-a'), b=fixture('client-b');
  assert.equal(a.connection.url,'ws://192.0.2.10:9001');
  assert.equal(a.connection.options.password,'fixture-code');
  assert.equal(a.subject.registerSuccess,false);
  await assert.rejects(a.subject.request('command','reply',{}),/DeviceNotRegistered/);
  a.subject.onReceiveMessage('elegoo/fixture-sn/api_status',JSON.stringify({method:'status',id:1,result:{}}));
  assert.equal(a.subject.registerSuccess,false,'telemetry alone is not registration');
  await register(a,{client_id:'client-a',error:'ok'});
  assert.equal(a.published[0].topic,'elegoo/fixture-sn/api_register');
  assert.equal(a.published[0].body.client_id,'client-a');
  assert.equal(a.subject.registerSuccess,true);
  assert.equal(b.subject.registerSuccess,false,'a second page owns a separate session');
  await assert.rejects(register(b,{client_id:'client-b',error:'firmware rejected registration'}),/firmware rejected/);
  assert.equal(b.subject.registerSuccess,false);
  await assert.rejects(b.subject.request('command','reply',{}),/DeviceNotRegistered/);
  await register(b,{client_id:'client-b',error:'already registered'});
  assert.equal(b.subject.registerSuccess,true,'a later valid reply recovers the same page');
  const c=fixture('client-c');
  const pending=c.subject.requestRegister('elegoo/fixture-sn/api_register','elegoo/fixture-sn/request-1/register_response',{});
  await new Promise(setImmediate);
  c.subject.onReceiveMessage('elegoo/other-sn/request-1/register_response',JSON.stringify({client_id:'client-c',error:'ok'}));
  c.subject.onReceiveMessage('elegoo/fixture-sn/request-1/register_response',JSON.stringify({client_id:'other-client',error:'ok'}));
  assert.equal(c.subject.registerSuccess,false,'foreign serial/client replies cannot register this page');
  // Run the actual registration timeout callback without a clock, socket or real delay.
  [...c.timers.values()][0]();
  await assert.rejects(pending,/RegisterTimeout/);
  assert.equal(c.subject.registerSuccess,false);
  a.subject.disconnect();b.subject.disconnect();c.subject.disconnect();
  completed=true;
  console.log('PASS: shipped CC2 register topic/auth, independent page state, gated commands, matching identity, firmware refusal, timeout and disconnect');
})().catch(error=>{console.error(error);process.exitCode=1;});
