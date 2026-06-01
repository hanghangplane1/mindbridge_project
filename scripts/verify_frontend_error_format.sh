#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

node <<'JS'
const fs = require('fs');
const vm = require('vm');

const source = fs.readFileSync('frontend/demo/app.js', 'utf8');
const match = source.match(/function formatAgentError\([^]*?\n}\n/);
if (!match) {
  throw new Error('formatAgentError function not found');
}

const context = {};
vm.createContext(context);
vm.runInContext(match[0], context);

const invalidKey = context.formatAgentError({
  code: -32000,
  message: 'run_model_stream: dashscope HTTP 401: {"code":"InvalidApiKey","message":"Invalid API-key provided."}',
});
if (!invalidKey.includes('DashScope API key is invalid')) {
  throw new Error('InvalidApiKey should produce an actionable DashScope key message');
}
if (!invalidKey.includes('scripts/start_demo_dashscope.sh')) {
  throw new Error('InvalidApiKey message should point to the DashScope startup script');
}
if (invalidKey.includes('sk-')) {
  throw new Error('InvalidApiKey message must not echo key-like secrets');
}

const generic = context.formatAgentError({ code: 123, message: 'plain failure' });
if (!generic.includes('plain failure')) {
  throw new Error('generic errors should preserve the provider message');
}

console.log('PASS: frontend error formatting validated');
JS
