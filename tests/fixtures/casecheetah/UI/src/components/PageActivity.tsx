import { createContext, useContext } from 'react';

/** 覆盖页保留挂载；portal 中的提示、下拉也应跟随所属页面的活动状态。 */
export const PageActivityContext = createContext(true);

export function usePageActive() {
  return useContext(PageActivityContext);
}
