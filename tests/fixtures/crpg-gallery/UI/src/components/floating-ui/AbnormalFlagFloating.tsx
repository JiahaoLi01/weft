import { motion } from 'framer-motion';
import type { FloatingUIInstance } from '@/stores/floating-ui-store';
import { ABNORMAL_FLAG_ICONS, ABNORMAL_FLAG_FALLBACK } from '@/config/abnormal-flags';
import { useAbnormalFlagLabel } from '@/i18n/battle-labels';

/**
 * Floating abnormal-status change —— `abnormal-flag` widget type 的渲染。
 *
 * 异常状态「增 / 删」的瞬时飘字,跟 DamageNumber 同一套 floating UI 机制(SpawnAtLocation +
 * TTL,policy 'hide')。区别于头顶血条上常驻的「当前状态 icon」(HealthBar 的 AbnormalFlags)——
 * 那个是快照,这个是「刚刚发生了变化」的事件反馈。
 *
 * 数据(从 instance.data 读,跟 `UAbnormalFlagFloatingViewModel` 反射对齐):
 *   - FlagId    string   异常状态 tag 的 leaf 名(如 "Reveal" / "Unmovable"),查 icon + label
 *   - bRemoved  bool     true → 移除(删除线动画);false → 新增(上浮淡入)
 *
 * C++ 端只在 stack 跨越 0↔1 边界时 spawn(见 BEAC::SpawnAbnormalFlagFloating):全新获得才弹
 * 新增,最后一层移除(归零)才弹删除。中间的 +1/-1 不弹。
 *
 * 动画(两套,跟 bRemoved 分支;总时长 ≈ 1.55-1.6s,对齐 C++ TTL=1.7s,留 buffer 给 framer 跑完):
 *   新增:pop-in → 持续上浮 → 末尾淡出。暖金色(获得感,不用霓虹绿)。
 *   移除:pop-in → 删除线从左到右划过(scaleX,GPU 合成)→ 静置 → 整体褪色下沉淡出。灰白色。
 *
 * 美学:古典 / 反拟物 —— 衬线字(var(--font-default))、暖金 / 灰白双色、羽化暗椭圆背衬(复用
 * HealthBar 名字那套,保证世界场景上的可读性),无发光 / 无 3D / 无霓虹。
 */

interface AbnormalFlagFloatingProps {
  instance: FloatingUIInstance;
}

// 暖金 = 获得;灰白 = 失去。色相差异 + 删除线 + 升/沉方向共同区分增删,不依赖霓虹红绿。
const INK_ADD = '#ecd9a6';
const INK_REMOVE = '#b9b2a4';

export function AbnormalFlagFloating({ instance }: AbnormalFlagFloatingProps) {
  const flagId = String(instance.data.FlagId ?? '');
  const bRemoved = Boolean(instance.data.bRemoved);

  const getLabel = useAbnormalFlagLabel();
  const label = getLabel(flagId); // 缺翻译 → null,只显示 icon
  const Icon = ABNORMAL_FLAG_ICONS[flagId] ?? ABNORMAL_FLAG_FALLBACK;

  const ink = bRemoved ? INK_REMOVE : INK_ADD;

  // 内容行(icon + 可选 label)+ 羽化暗椭圆背衬。两套动画共用同一内容,只切 ink / 动画 timeline。
  const content = (
    <div className="relative flex items-center justify-center gap-1 px-1 leading-none">
      {/* 羽化暗椭圆 —— 复用 HealthBar 名字的可读性处理(默认 ellipse + 72% 收口,Ultralight 友好)。 */}
      <span
        className="absolute pointer-events-none"
        style={{
          inset: '-3px -16px',
          background:
            'radial-gradient(ellipse at center, rgba(0,0,0,0.6) 0%, rgba(0,0,0,0.46) 36%, rgba(0,0,0,0.2) 58%, rgba(0,0,0,0) 72%)',
        }}
        aria-hidden
      />
      <Icon
        size={14}
        strokeWidth={1.5}
        color={ink}
        style={{ filter: 'drop-shadow(0 1px 0.5px rgba(0,0,0,0.95))', position: 'relative' }}
      />
      {label && (
        <span
          className="relative truncate"
          style={{
            maxWidth: 120,
            color: ink,
            fontFamily: 'var(--font-default)',
            fontSize: 13,
            fontWeight: 600,
            letterSpacing: 0.4,
            textShadow: '0 1px 2px rgba(0,0,0,0.9), 0 0 2px rgba(0,0,0,0.7)',
          }}
        >
          {label}
        </span>
      )}

      {/* 删除线 —— 仅移除时渲染。从左到右划过整行(icon+label),scaleX(GPU 合成,零 layout)。
          delay 让它在内容 pop-in 之后才开始划,顺序读起来是「出现 → 被划掉」。 */}
      {bRemoved && (
        <motion.span
          className="absolute pointer-events-none"
          style={{
            left: 2,
            right: 2,
            top: '50%',
            height: 2,
            transformOrigin: 'left center',
            background: INK_REMOVE,
            boxShadow: '0 0 1px rgba(0,0,0,0.9)',
            borderRadius: 1,
          }}
          initial={{ scaleX: 0, opacity: 0.95 }}
          animate={{ scaleX: 1 }}
          transition={{ duration: 0.32, delay: 0.2, ease: 'easeOut' }}
          aria-hidden
        />
      )}
    </div>
  );

  // 新增:暖金,持续上浮 + 末尾淡出。
  if (!bRemoved) {
    return (
      <motion.div
        className="select-none pointer-events-none"
        initial={{ opacity: 0, scale: 0.7, y: 6 }}
        animate={{ opacity: [0, 1, 1, 0], scale: [0.7, 1, 1, 1], y: [6, -6, -16, -24] }}
        transition={{ duration: 1.6, times: [0, 0.12, 0.7, 1], ease: ['easeOut', 'linear', 'easeIn'] }}
      >
        {content}
      </motion.div>
    );
  }

  // 移除:灰白,静置后整体褪色下沉淡出(删除线在 content 内部自播)。
  return (
    <motion.div
      className="select-none pointer-events-none"
      initial={{ opacity: 0, scale: 0.8, y: -2 }}
      animate={{ opacity: [0, 1, 1, 0], scale: [0.8, 1, 1, 0.96], y: [-2, 0, 0, 10] }}
      transition={{ duration: 1.55, times: [0, 0.1, 0.72, 1], ease: ['easeOut', 'linear', 'easeIn'] }}
    >
      {content}
    </motion.div>
  );
}
