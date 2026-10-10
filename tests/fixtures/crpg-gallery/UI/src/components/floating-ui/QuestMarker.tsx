import { motion } from 'framer-motion';
import { MapPin, ChevronRight } from 'lucide-react';
import type { FloatingUIInstance } from '@/stores/floating-ui-store';

/**
 * Quest Marker —— `quest-marker` widget type 的渲染。
 *
 * **当前状态:placeholder**。C++ 端 ViewModel 尚未定义,数据 schema 未敲定。这里先用
 * 一个固定的 MapPin icon + 可选 label 占位,等接 C++ 时再按 VM 字段扩展(quest title /
 * objective progress / quest icon variant 等)。
 *
 * Off-screen 行为:走 `edge-clamp` policy(见 floating-ui-types.ts)。on-screen 时
 * 渲染本组件 `QuestMarker`,off-screen 时 Overlay 切到 `QuestMarkerEdgeIndicator`
 * (简化版只有箭头),贴在 viewport 边缘指向目标。
 *
 * VM 字段(未来):
 *   - Label: string         marker 旁边的小字标签(quest 名 / 距离等)
 *   - Variant: string       icon variant key(main / side / treasure 等)
 *   - PriorityLevel: number 用来调整颜色 / 大小
 */
interface QuestMarkerProps {
  instance: FloatingUIInstance;
}

export function QuestMarker({ instance }: QuestMarkerProps) {
  const label = String(instance.data?.Label ?? '');

  return (
    <motion.div
      className="flex items-center gap-1.5 select-none pointer-events-none"
      // 入场轻微 bounce —— marker 出现时弹一下提示存在
      initial={{ scale: 0.6, opacity: 0 }}
      animate={{ scale: 1, opacity: 1 }}
      transition={{ type: 'spring', stiffness: 400, damping: 22 }}
    >
      <div
        className="size-7 rounded-full flex items-center justify-center"
        style={{
          background: 'radial-gradient(circle at 30% 30%, rgba(240,208,128,0.35), rgba(20,15,10,0.7) 70%)',
          border: '1.5px solid rgba(240,208,128,0.75)',
          boxShadow: '0 0 8px rgba(212,168,67,0.45), inset 0 0 4px rgba(0,0,0,0.6)',
        }}
      >
        <MapPin className="size-4 text-gold-light" strokeWidth={2} fill="currentColor" fillOpacity={0.3} />
      </div>
      {label && (
        <div
          className="text-xs text-parchment font-[family-name:var(--font-display)] px-2 py-0.5 rounded"
          style={{
            background: 'rgba(20,15,10,0.7)',
            border: '1px solid rgba(212,168,67,0.4)',
            textShadow: '0 1px 2px rgba(0,0,0,0.8)',
          }}
        >
          {label}
        </div>
      )}
    </motion.div>
  );
}

/**
 * Off-screen 边缘指示器 —— 简化版 marker,只有箭头本体。
 *
 * 渲染时,Overlay 已经处理好定位 + 旋转(`transform: translate(x,y) rotate(deg)`),
 * 这里只画"以 (0,0) 为中心、面朝 +x 方向" 的箭头形状。CSS rotate 把它转到正确方向。
 *
 * 箭头本体用 ChevronRight icon + 圆形底纹,跟 on-screen 版的 MapPin 同色系,
 * 玩家一眼看出来"是同一个东西,只是被推到屏幕外了"。
 */
export function QuestMarkerEdgeIndicator({ instance }: { instance: FloatingUIInstance }) {
  // instance 当前没消费(箭头本体跟 instance 数据无关),保留参数让所有 FloatingUIComponent
  // 走同一签名(`floating-ui-types.ts` 的 FloatingUIComponent 类型)。后期若想根据 instance.data
  // 改箭头变体(主任务金 / 副任务蓝)直接读 instance.data.Variant 即可,无需改 wiring。
  // `void instance` 显式标记"我看到这个参数了,但故意不用",避免 unused-var lint。
  void instance;
  return (
    <div
      className="size-7 rounded-full flex items-center justify-center pointer-events-none"
      style={{
        background: 'radial-gradient(circle at 30% 30%, rgba(240,208,128,0.45), rgba(20,15,10,0.85) 70%)',
        border: '1.5px solid rgba(240,208,128,0.85)',
        boxShadow: '0 0 10px rgba(212,168,67,0.6), inset 0 0 4px rgba(0,0,0,0.6)',
      }}
    >
      {/* ChevronRight 默认指 +x,跟 Overlay 给的 rotation 0° 约定一致。
          stroke 加粗让它在边缘小尺寸下仍可读。 */}
      <ChevronRight className="size-5 text-gold-light" strokeWidth={3} />
    </div>
  );
}
