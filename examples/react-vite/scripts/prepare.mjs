import { copyFileSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
mkdirSync(resolve(import.meta.dirname, '../public'), { recursive: true });
copyFileSync(resolve(import.meta.dirname, '../../../third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf'), resolve(import.meta.dirname, '../public/probe-roboto.ttf'));
