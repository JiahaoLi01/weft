import { lazy, Suspense } from 'react';
import { BackButton } from '@/components/ui/BackButton';

const TestGallery = lazy(() =>
  import('@/components/debug/TestGallery').then(m => ({ default: m.TestGallery }))
);

export function TestGalleryPage() {
  return (
    <>
      <BackButton />
      <Suspense fallback={
        <div className="w-full h-full flex items-center justify-center text-zinc-500 text-sm">
          Loading test gallery...
        </div>
      }>
        <TestGallery />
      </Suspense>
    </>
  );
}
