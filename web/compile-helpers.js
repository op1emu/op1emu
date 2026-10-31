// The emulator's side of compile-worker.js (web/README.md), for worker.js
// and tools/run-node.mjs: promotions to tier 1 go to the tier Worker
// (op1_remote_tier, Module.bcoreSubmit) with the emulator's compiled module
// (module), its glue (js) and LLVM's deferred module (deferred, a URL), and
// their code comes back between steps (op1_tier_delivered). If that Worker
// dies, the promotions under way fail and no more are made: blocks stay at
// tier 0. spawn() makes a Worker of compile-worker.js, as {post(message,
// transfer), onMessage(handler), onError(handler), terminate()}; onError's
// handler is for the Worker's end too, where nothing else says it ended.
export function attachTierWorker(op1, { spawn, module, js, deferred }) {
  const counts = { jobs: 0, codes: 0, fails: 0, dead: "" };
  const worker = spawn();
  const inflight = new Set();
  let dead = false;
  op1.bcoreModules = new Map();
  // Each ticket answered once: a job the Worker fails after it died was
  // failed already.
  const fail = (ticket) => {
    if (!inflight.delete(ticket)) return;
    counts.fails++;
    op1._op1_tier_delivered(ticket, 0, 0, 0);
  };
  const die = (why) => {
    if (dead) return;
    dead = true;
    counts.dead = why;
    console.error(`op1: the tier-1 Worker stopped: ${why}`);
    for (const ticket of [...inflight]) fail(ticket);
  };
  worker.onMessage((message) => {
    if (message.type === "code") {
      if (!inflight.delete(message.ticket)) return;
      counts.codes++;
      op1.bcoreModules.set(message.ticket, message.module);
      const name = op1.stringToNewUTF8(message.name);
      op1._op1_tier_delivered(message.ticket, 1, name, message.fallthrough);
      op1._free(name);
    } else if (message.type === "fail") {
      fail(message.ticket);
    } else if (message.type === "dead") {
      die(message.why);
    }
  });
  worker.onError((error) => die(String(error?.message ?? error)));
  worker.post({ type: "init", module, js, deferred });
  op1.bcoreSubmit = (ticket, job) => {
    if (dead) return false;
    counts.jobs++;
    inflight.add(ticket);
    worker.post({ type: "job", ticket, job }, [job.buffer]);
    return true;
  };
  op1._op1_remote_tier(1);
  return {
    counts,
    stop() {
      dead = true;  // no more jobs, and its end is no failure
      op1._op1_remote_tier(0);
      delete op1.bcoreSubmit;
      worker.terminate();
    },
  };
}
