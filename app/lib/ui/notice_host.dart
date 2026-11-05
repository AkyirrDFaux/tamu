import 'dart:async';

import 'package:flutter/material.dart';

import '../core/notice.dart';
import 'theme.dart';

/// `MaterialApp.builder` for the app: every route, including dialogs, gets the one
/// notice strip below it.
Widget noticeHostBuilder(BuildContext context, Widget? child) =>
    NoticeHost(child: child ?? const SizedBox.shrink());

/// The app's single visible notice, laid out as a strip **below** [child].
///
/// The strip is a sibling of the routes in a [Column], not an overlay: the page gives it
/// the height it takes, so nothing is ever underneath it and the strip can only take a
/// pointer event inside its own row. That is the point of the shape - a snack bar floats
/// over the bottom of the page (measured: it covered the last two rows of the Backup page
/// at both 360x640 and 800x360), so the control under it stays unreachable until the timer
/// runs out. A dialog route cannot cover the strip either, which is what makes a notice
/// raised from a dialog (the subscription editor's save failure) visible at all.
///
/// The strip animates its height, so the page reflows rather than jumps. It disappears on
/// the user's close control, on a horizontal swipe, or when its own timer runs out.
class NoticeHost extends StatefulWidget {
  const NoticeHost({super.key, required this.child});

  final Widget child;

  @override
  State<NoticeHost> createState() => _NoticeHostState();
}

class _NoticeHostState extends State<NoticeHost> {
  final NoticeCenter _center = NoticeCenter.instance;

  /// The id of the notice whose timer is running, so a rebuild or a repeated
  /// notification for the same line does not restart its four seconds.
  int? _timed;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _center.addListener(_onNotice);
    _center.attachHost();
    _onNotice();
  }

  @override
  void dispose() {
    _timer?.cancel();
    _center.removeListener(_onNotice);
    _center.detachHost();
    super.dispose();
  }

  void _onNotice() {
    final notice = _center.current;
    if (notice?.id == _timed) return;
    _timer?.cancel();
    _timer = null;
    _timed = notice?.id;
    if (notice != null) {
      _timer = Timer(notice.duration, () => _center.dismiss(notice));
    }
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final notice = _center.current;
    return Column(
      children: [
        Expanded(child: widget.child),
        AnimatedSize(
          duration: const Duration(milliseconds: 180),
          curve: Curves.easeOut,
          alignment: Alignment.topCenter,
          child: notice == null
              ? const SizedBox(width: double.infinity)
              : NoticeBand(
                  notice: notice,
                  onDismiss: () => _center.dismiss(notice),
                ),
        ),
      ],
    );
  }
}

/// How much room a notice takes from the page. One API, two explicit variants: the shape
/// is a property of the window, not of the notice, because what differs is how much height
/// the page can afford to give up.
enum NoticeBandShape {
  /// A window with height to spare: the message wraps at body size, with the close control
  /// on its own trailing edge.
  card,

  /// A short window (a landscape phone, 800x360): the whole strip stays one compact row at
  /// the small text size, because every pixel it takes is a pixel the page loses.
  strip;

  /// Lines a message may take, per shape. A guard, not a layout: a notice is one sentence, and
  /// the longest one the app raises (a refusal that names the step to take, "open a pairing
  /// window from USB") is ~70 characters - about five lines in the wide test font at 360 px and
  /// two at 800. The cap only exists so a pathological line cannot take the page's whole height;
  /// real copy must not reach it, or the guidance would be cut with an ellipsis.
  static int maxLines(NoticeBandShape shape) => switch (shape) {
    NoticeBandShape.card => 6,
    NoticeBandShape.strip => 3,
  };

  /// The height below which a window gets [strip]. The axis is height, not width: a short
  /// window is one of the app's landscape layouts (its two short windows are 640x360 and
  /// 800x360), where the card's extra padding would cost the page more than it buys. A
  /// window with height to spare - a phone held upright, a desktop window - keeps the card.
  static const double stripBelow = 480;

  /// The shape [context]'s window takes.
  static NoticeBandShape of(BuildContext context) =>
      MediaQuery.sizeOf(context).height < stripBelow ? strip : card;
}

/// One notice: an icon for its tone, its line, and the close control. Swiping it
/// sideways dismisses it too - the gesture the platform's own banners take.
class NoticeBand extends StatelessWidget {
  const NoticeBand({
    super.key,
    required this.notice,
    required this.onDismiss,
    this.shape,
  });

  final AppNotice notice;
  final VoidCallback onDismiss;

  /// Null asks the window ([NoticeBandShape.of]).
  final NoticeBandShape? shape;

  static const String closeLabel = 'Close notice';

  /// The close control's key. The strip is drawn outside the [Navigator], so its control
  /// carries no tooltip (a tooltip needs an [Overlay] ancestor) and a test reaches it by key.
  static const Key closeKey = Key('notice-close');

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final shape = this.shape ?? NoticeBandShape.of(context);
    final compact = shape == NoticeBandShape.strip;
    final (IconData icon, Color ink) = switch (notice.tone) {
      NoticeTone.info => (Icons.info_outline, kInfo),
      NoticeTone.success => (Icons.check_circle_outline, kSuccess),
      NoticeTone.warning => (Icons.warning_amber_outlined, kWarning),
      NoticeTone.error => (Icons.error_outline, kError),
    };
    return Dismissible(
      key: ValueKey<int>(notice.id),
      direction: DismissDirection.horizontal,
      onDismissed: (_) => onDismiss(),
      child: Semantics(
        container: true,
        liveRegion: true,
        child: Material(
          color: kSurfaceAlt,
          child: SafeArea(
            top: false,
            child: DecoratedBox(
              decoration: const BoxDecoration(
                border: Border(top: BorderSide(color: kLine)),
              ),
              child: Padding(
                padding: compact
                    ? const EdgeInsets.fromLTRB(
                        Spacing.lg,
                        Spacing.xs,
                        Spacing.xs,
                        Spacing.xs,
                      )
                    : const EdgeInsets.fromLTRB(
                        Spacing.xl,
                        Spacing.md,
                        Spacing.sm,
                        Spacing.md,
                      ),
                child: Row(
                  children: [
                    Icon(icon, size: IconSize.md, color: ink),
                    const SizedBox(width: Spacing.lg),
                    Expanded(
                      child: Text(
                        notice.message,
                        style: compact
                            ? theme.textTheme.bodySmall
                            : theme.textTheme.bodyMedium,
                        maxLines: NoticeBandShape.maxLines(shape),
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                    const SizedBox(width: Spacing.sm),
                    Semantics(
                      label: closeLabel,
                      button: true,
                      child: IconButton(
                        key: closeKey,
                        icon: const Icon(Icons.close, size: IconSize.lg),
                        onPressed: onDismiss,
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
