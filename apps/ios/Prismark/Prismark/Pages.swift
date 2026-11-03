import Observation
import SwiftUI

/// The screens opened on top of a section. iOS's own stack rounds the corners of a screen while it slides, so the app
/// draws its own: square edges and a seam, like everything else.
@Observable final class Pages {
    var path: [AnyHashable] = []

    func push(_ page: AnyHashable) {
        withAnimation(Pages.slide) { path.append(page) }
    }

    func pop() {
        withAnimation(Pages.slide) { _ = path.popLast() }
    }

    static let slide = Animation.easeOut(duration: 0.28)
}

/// A section's first screen, and the screens opened on top of it.
struct PageStack<Root: View>: View {
    @State private var pages = Pages()
    @State private var drag: CGFloat = 0
    let destination: (AnyHashable) -> AnyView
    @ViewBuilder let root: Root

    var body: some View {
        GeometryReader { geo in
            let width = geo.size.width
            let open = pages.path.count
            // The screen below moves a quarter of the way as the top one slides over it.
            let below = -width / 4 * (1 - drag / width)

            ZStack(alignment: .leading) {
                root
                    .frame(width: width, height: geo.size.height)
                    .offset(x: open > 0 ? below : 0)
                    .allowsHitTesting(open == 0)
                    .accessibilityHidden(open > 0)

                ForEach(Array(pages.path.enumerated()), id: \.offset) { index, page in
                    let top = index == open - 1

                    destination(page)
                        .frame(width: width, height: geo.size.height)
                        .overlay(alignment: .leading) {
                            if top { edgeSwipe(width: width) }
                        }
                        .background(alignment: .leading) {
                            Color.ground.frame(width: 6).offset(x: -6)
                        }
                        .offset(x: top ? drag : (open - index == 2 ? below : 0))
                        .allowsHitTesting(top)
                        .accessibilityHidden(!top)
                        .accessibilityAction(.escape) { pages.pop() }
                        .transition(.move(edge: .trailing))
                        .zIndex(Double(index + 1))
                }
            }
            .frame(width: width, height: geo.size.height, alignment: .leading)
            .clipped()
        }
        .ignoresSafeArea(.container, edges: .vertical)
        .environment(pages)
    }

    /// A thin strip along the left edge: the screen follows the finger, and goes back past a third of the way.
    private func edgeSwipe(width: CGFloat) -> some View {
        Color.clear
            .frame(width: 18)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 4, coordinateSpace: .global)
                    .onChanged { drag = max(0, $0.translation.width) }
                    .onEnded { end in
                        guard max(end.translation.width, end.predictedEndTranslation.width) > width / 3 else {
                            withAnimation(Pages.slide) { drag = 0 }
                            return
                        }
                        withAnimation(Pages.slide) {
                            drag = width
                        } completion: {
                            // The screen is already off to the side, so it goes without a second slide.
                            var still = Transaction()
                            still.disablesAnimations = true
                            withTransaction(still) {
                                _ = pages.path.popLast()
                                drag = 0
                            }
                        }
                    }
            )
    }
}

/// A row or tile that opens a screen.
struct PageLink<Label: View>: View {
    @Environment(Pages.self) private var pages
    let page: AnyHashable
    @ViewBuilder let label: Label

    init<Page: Hashable>(_ page: Page, @ViewBuilder label: () -> Label) {
        self.page = AnyHashable(page)
        self.label = label()
    }

    var body: some View {
        Button { pages.push(page) } label: { label }
    }
}
