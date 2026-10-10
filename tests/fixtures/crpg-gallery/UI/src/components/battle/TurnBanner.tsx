/**
 * TurnBanner —— 轮次推进横幅(中上方),取代旧 RoundAnnouncement 飘字。
 *
 * 内容:阵营纹章 + 阵营名 + 当前回合数 —— 旧飘字只有回合数,这里是它的超集。
 * 数据流:
 *   C++ BattleUIDispatcher {SyncCurrentState | TurnAdvanced} → setBattleTurn → store.currentTurn
 *   C++ {SyncCurrentState | RoundAdvanced} → setBattleRound → store.currentRound
 *   C++ OnActivated → setBattleSession → store.session.factions(tag → 名字/纹章/主题色)
 *   本组件对 (factionTag, turnIndex, round) 签名做前值对比,变化即弹横幅。
 *
 * 视觉:无框"铭刻"风(跟浮空血条铭牌 / 移动距离标签同语言)—— 羽化椭圆暗色衬底,
 * 无实体面板盒;纹章 + 阵营名染阵营主题色(没配置走羊皮纸 ink),回合数走 subtitle
 * 混合栈(Cinzel 雕花数字)。入场:纹章 spring 落定 → 两侧黄铜细线向外展开 →
 * 阵营名浮现 → 回合数淡入;hold 后整体淡出。
 *
 * 挂载:**只在 BattleSessionOverlay 内**(battle session active 时存在)。组件挂载瞬间
 * 如果已有有效轮次(开局 SyncCurrentState 先于 session 信号落 store),前值对比首轮
 * 即触发 —— 战斗开场自然弹"第 1 回合 / 先手阵营"横幅,无需额外开场逻辑。
 */

import { createElement, useEffect, useState } from 'react';
import { AnimatePresence, motion } from 'framer-motion';
import { useBattleStore } from '@/stores/battle-store';
import { useRoundAnnouncementText } from '@/i18n/battle-labels';
import { getFactionEmblem, resolveFaction } from '@/config/faction-emblems';
import { brass, NAME_INK } from '@/config/battle-palette';

/** 入场动画总长(纹章 spring + 文字浮现)≈ 0.9s,之后 hold,再 exit。 */
const HOLD_MS = 2200;
const ENTRANCE_MS = 900;

interface BannerSnapshot {
  /** 自增 key —— AnimatePresence 识别"新一条"重播入场 */
  key: number;
  factionTag: string;
  round: number;
}

export function TurnBanner() {
  const factions = useBattleStore((s) => s.session.factions);
  const currentTurn = useBattleStore((s) => s.currentTurn);
  const currentRound = useBattleStore((s) => s.currentRound);
  const getRoundText = useRoundAnnouncementText();

  // 轮次签名前值对比(render 阶段 setState —— React 官方"对比前一次渲染"模式,
  // 同 HpBar / 浮空血条的 trail 检测)。签名变化 → 冻结一份横幅快照;后续 store
  // 更新不影响已显示内容。turnIndex 进签名让"同阵营连续两轮"也能重播。
  const sig = currentTurn.factionTag
    ? `${currentTurn.factionTag}|${currentTurn.turnIndex}|${currentRound}`
    : '';
  const [prevSig, setPrevSig] = useState('');
  const [banner, setBanner] = useState<BannerSnapshot | null>(null);
  if (sig !== prevSig) {
    setPrevSig(sig);
    if (sig) {
      setBanner((prev) => ({
        key: (prev?.key ?? 0) + 1,
        factionTag: currentTurn.factionTag,
        round: currentRound,
      }));
    }
  }

  // hold 结束自动退场(AnimatePresence 跑 exit 淡出)。banner 替换时旧 timer 被 cleanup。
  useEffect(() => {
    if (!banner) {
      return;
    }
    const handle = setTimeout(() => setBanner(null), ENTRANCE_MS + HOLD_MS);
    return () => clearTimeout(handle);
  }, [banner]);

  const resolved = banner ? resolveFaction(banner.factionTag, factions) : null;
  // 阵营主题色给纹章 + 名字染色;未配置走羊皮纸 ink(NAME_INK,同浮空铭牌)
  const ink = resolved?.color || NAME_INK;
  // createElement 走 dynamic-icon rendering(同 battle-icons 的用法注释)
  const emblemEl = resolved
    ? createElement(getFactionEmblem(resolved.emblemId), { size: 52 })
    : null;

  return (
    <div className="absolute top-[10vh] inset-x-0 flex justify-center pointer-events-none select-none">
      <AnimatePresence>
        {banner && resolved && (
          <motion.div
            key={banner.key}
            className="relative flex flex-col items-center"
            initial={{ opacity: 0, y: -14 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, transition: { duration: 0.5, ease: 'easeOut' } }}
            transition={{ duration: 0.35, ease: [0.16, 1, 0.3, 1] }}
          >
            {/* 羽化暗色衬底 —— 配方同浮空血条铭牌(Ultralight 忽略 ellipse size 关键字,
                farthest-corner + 72% 归零模拟内切椭圆;宽扁负 inset 贴合内容块) */}
            <span
              aria-hidden
              className="absolute pointer-events-none"
              style={{
                inset: '-20px -72px',
                background:
                  'radial-gradient(ellipse at center, rgba(0,0,0,0.6) 0%, rgba(0,0,0,0.44) 34%, rgba(0,0,0,0.18) 56%, rgba(0,0,0,0) 72%)',
              }}
            />

            {/* 阵营纹章 —— spring 落定,染主题色 */}
            <motion.div
              className="relative"
              style={{ color: ink, filter: 'drop-shadow(0 1px 1px rgba(0,0,0,0.8))' }}
              initial={{ scale: 0.55, opacity: 0 }}
              animate={{ scale: 1, opacity: 1 }}
              transition={{ type: 'spring', stiffness: 320, damping: 19, delay: 0.05 }}
            >
              {emblemEl}
            </motion.div>

            {/* 阵营名 —— 两侧黄铜细线向外展开(── 名字 ──,雕版铭牌母题) */}
            <div className="relative flex items-center mt-1.5">
              <motion.span
                aria-hidden
                className="shrink-0 origin-right"
                style={{
                  width: 56,
                  height: 1,
                  marginRight: 10,
                  background: `linear-gradient(90deg, ${brass(0)}, ${brass(0.65)})`,
                }}
                initial={{ scaleX: 0, opacity: 0 }}
                animate={{ scaleX: 1, opacity: 1 }}
                transition={{ delay: 0.22, duration: 0.45, ease: [0.16, 1, 0.3, 1] }}
              />
              <motion.span
                className="whitespace-nowrap"
                style={{
                  color: ink,
                  fontFamily: 'var(--font-display)',
                  fontSize: 24,
                  fontWeight: 600,
                  letterSpacing: '0.14em',
                  lineHeight: 1.2,
                  textShadow: '0 1px 2px rgba(0,0,0,0.9), 0 0 2px rgba(0,0,0,0.7)',
                }}
                initial={{ opacity: 0, y: 6 }}
                animate={{ opacity: 1, y: 0 }}
                transition={{ delay: 0.18, duration: 0.4, ease: [0.16, 1, 0.3, 1] }}
              >
                {resolved.name}
              </motion.span>
              <motion.span
                aria-hidden
                className="shrink-0 origin-left"
                style={{
                  width: 56,
                  height: 1,
                  marginLeft: 10,
                  background: `linear-gradient(270deg, ${brass(0)}, ${brass(0.65)})`,
                }}
                initial={{ scaleX: 0, opacity: 0 }}
                animate={{ scaleX: 1, opacity: 1 }}
                transition={{ delay: 0.22, duration: 0.45, ease: [0.16, 1, 0.3, 1] }}
              />
            </div>

            {/* 回合数 —— subtitle 混合栈(Cinzel 雕花数字),小字距拉开,羊皮纸暗色 */}
            <motion.div
              className="relative mt-1"
              style={{
                fontFamily: 'var(--font-subtitle)',
                fontVariantNumeric: 'tabular-nums',
                fontSize: 13,
                letterSpacing: '0.3em',
                // letterSpacing 会在末字符后也加间距,padding-left 抵消让视觉居中
                paddingLeft: '0.3em',
                color: '#d4c4a0',
                textShadow: '0 1px 2px rgba(0,0,0,0.9)',
              }}
              initial={{ opacity: 0 }}
              animate={{ opacity: 1 }}
              transition={{ delay: 0.45, duration: 0.45, ease: 'easeOut' }}
            >
              {getRoundText(banner.round)}
            </motion.div>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}
