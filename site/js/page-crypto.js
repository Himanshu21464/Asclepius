(() => {
  'use strict';
  const $   = (s) => document.querySelector(s);
  const enc = new TextEncoder();
  const HEX = (buf) => Array.from(new Uint8Array(buf))
    .map((b) => b.toString(16).padStart(2, '0')).join('');

  // ─── 01. hash visualizer ───────────────────────────────────────────
  async function refreshHashes() {
    const v = $('#cr-hash-in').value;
    const data = enc.encode(v);
    const [h256, h384, h512] = await Promise.all([
      crypto.subtle.digest('SHA-256', data),
      crypto.subtle.digest('SHA-384', data),
      crypto.subtle.digest('SHA-512', data),
    ]);
    $('#cr-h256').textContent = HEX(h256);
    $('#cr-h384').textContent = HEX(h384);
    $('#cr-h512').textContent = HEX(h512);
  }
  $('#cr-hash-in').addEventListener('input', refreshHashes);
  refreshHashes();

  // ─── 02. Ed25519 sign / verify ─────────────────────────────────────
  let kp = null, alg = 'Ed25519', signature = null;
  async function genKey() {
    kp = null; signature = null;
    $('#cr-key-display').style.display = 'none';
    $('#cr-sig').textContent = '—';
    $('#cr-verify-out').textContent = 'click sign, then verify';
    $('#cr-verify-out').className = 'cr-result';
    try {
      kp = await crypto.subtle.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']);
      alg = 'Ed25519';
    } catch (e) {
      kp = await crypto.subtle.generateKey(
        { name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
      alg = 'ECDSA-P256';
    }
    let pkBytes, skBytes;
    try { pkBytes = new Uint8Array(await crypto.subtle.exportKey('raw',  kp.publicKey)); }
    catch (e) { pkBytes = new Uint8Array(await crypto.subtle.exportKey('spki', kp.publicKey)); }
    skBytes = new Uint8Array(await crypto.subtle.exportKey('pkcs8', kp.privateKey));
    const pkHex = HEX(pkBytes);
    $('#cr-alg-display').textContent = alg;
    $('#cr-keyid').textContent       = pkHex.slice(0, 16);
    $('#cr-pk').textContent          = pkHex;
    $('#cr-sk').textContent          = HEX(skBytes);
    $('#cr-key-display').style.display = '';
    $('#cr-key-alg').textContent     = `${alg} · key ready`;
  }
  $('#cr-keygen').addEventListener('click', genKey);

  $('#cr-sign-btn').addEventListener('click', async () => {
    if (!kp) { alert('generate a keypair first'); return; }
    const msg  = enc.encode($('#cr-sign-msg').value);
    const algo = alg === 'Ed25519' ? { name: 'Ed25519' } : { name: 'ECDSA', hash: 'SHA-256' };
    signature  = new Uint8Array(await crypto.subtle.sign(algo, kp.privateKey, msg));
    $('#cr-sig').textContent = HEX(signature);
    $('#cr-verify-out').textContent = 'signed · click verify to check';
    $('#cr-verify-out').className = 'cr-result';
  });

  $('#cr-verify-btn').addEventListener('click', async () => {
    if (!kp || !signature) { alert('sign a message first'); return; }
    const msg = enc.encode($('#cr-sign-msg').value);
    const algo = alg === 'Ed25519' ? { name: 'Ed25519' } : { name: 'ECDSA', hash: 'SHA-256' };
    const ok  = await crypto.subtle.verify(algo, kp.publicKey, signature, msg);
    const out = $('#cr-verify-out');
    if (ok) {
      out.textContent = `verified · ${alg} · ${signature.length} bytes`;
      out.className   = 'cr-result is-ok';
    } else {
      out.textContent = 'INVALID · message bytes do not match the signed bytes';
      out.className   = 'cr-result is-bad';
    }
  });

  $('#cr-tamper-btn').addEventListener('click', () => {
    const ta = $('#cr-sign-msg');
    const v  = ta.value;
    if (!v.length) return;
    const i = Math.floor(Math.random() * v.length);
    const c = v.charCodeAt(i);
    ta.value = v.slice(0, i) + String.fromCharCode(c ^ 1) + v.slice(i + 1);
    $('#cr-verify-out').textContent = `mutated 1 byte at index ${i} · click verify to see what happens`;
    $('#cr-verify-out').className = 'cr-result';
  });

  // ─── 03. chain forge ───────────────────────────────────────────────
  let chain = [];
  let chainKp = null;
  let chainAlg = 'Ed25519';

  async function buildChain() {
    try { chainKp = await crypto.subtle.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']); chainAlg = 'Ed25519'; }
    catch (e) { chainKp = await crypto.subtle.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']); chainAlg = 'ECDSA-P256'; }
    chain = [];
    let prev = '0'.repeat(64);
    for (let i = 1; i <= 5; i++) {
      const body = JSON.stringify({ seq: i, actor: 'clinician:bench', payload: `entry ${i}` });
      const payloadHash = HEX(await crypto.subtle.digest('SHA-256', enc.encode(body)));
      const signInput   = enc.encode(`${i}|${prev}|${payloadHash}|${body}`);
      const algo = chainAlg === 'Ed25519' ? { name: 'Ed25519' } : { name: 'ECDSA', hash: 'SHA-256' };
      const sig = HEX(new Uint8Array(await crypto.subtle.sign(algo, chainKp.privateKey, signInput)));
      const entryHash = HEX(await crypto.subtle.digest('SHA-256',
        enc.encode(`${i}|${prev}|${payloadHash}|${body}|${sig}`)));
      chain.push({ seq: i, prev, body, payloadHash, sig, entryHash });
      prev = entryHash;
    }
    renderChain();
    $('#cr-cverify-out').textContent = 'fresh chain built · 5 rows · click verify';
    $('#cr-cverify-out').className = 'cr-result';
  }

  function renderChain() {
    $('#cr-chain').innerHTML = chain.map((e) => `
        <div class="cr-key-row" style="border:1px solid var(--rule-soft);margin-bottom:.4rem;border-radius:5px">
          <b>seq ${String(e.seq).padStart(2,'0')}</b>
          <code><span style="color:var(--ink-mute)">prev</span> ${e.prev.slice(0,12)}…
                · <span style="color:var(--ink-mute)">body</span> ${e.body}
                <br><span style="color:var(--ink-mute)">hash</span> ${e.entryHash.slice(0,16)}…
                · <span style="color:var(--ink-mute)">sig</span>  ${e.sig.slice(0,16)}…</code>
        </div>`).join('');
  }

  $('#cr-build').addEventListener('click', buildChain);

  $('#cr-forge').addEventListener('click', () => {
    if (!chain.length) { alert('build a chain first'); return; }
    chain[2].body = JSON.stringify({ seq: 3, actor: 'attacker', payload: 'tampered!' });
    renderChain();
    $('#cr-cverify-out').textContent =
      'row 3 body modified · the original signature still covers the OLD bytes — verify will catch it';
    $('#cr-cverify-out').className = 'cr-result';
  });

  $('#cr-cverify').addEventListener('click', async () => {
    if (!chain.length) { alert('build a chain first'); return; }
    const out = $('#cr-cverify-out');
    let prev = '0'.repeat(64);
    for (const e of chain) {
      if (e.prev !== prev) {
        out.textContent = `INVALID · chain break at seq ${e.seq}`;
        out.className   = 'cr-result is-bad';
        return;
      }
      const recomputedPayload = HEX(await crypto.subtle.digest('SHA-256', enc.encode(e.body)));
      if (recomputedPayload !== e.payloadHash) {
        out.textContent = `INVALID · payload hash mismatch at seq ${e.seq}`;
        out.className   = 'cr-result is-bad';
        return;
      }
      const recomputedEntry = HEX(await crypto.subtle.digest('SHA-256',
        enc.encode(`${e.seq}|${e.prev}|${e.payloadHash}|${e.body}|${e.sig}`)));
      prev = recomputedEntry;
    }
    out.textContent = `OK · 5 entries · all hashes match · all signatures hold`;
    out.className   = 'cr-result is-ok';
  });
})();
