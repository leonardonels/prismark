// The phone wallet needs each ticket's height: see "Phone" at the end of styles.css.
const tickets = new ResizeObserver((entries) => {
    entries.forEach((entry) => {
        entry.target.style.setProperty('--h', entry.target.offsetHeight + 'px');
    });
});

document.querySelectorAll('.deck > .panel').forEach((panel) => tickets.observe(panel));

// Each animation waits, paused, until its element is on screen, and plays once.
const watcher = new IntersectionObserver((entries) => {
    entries.forEach((entry) => {
        if (entry.isIntersecting) {
            entry.target.classList.add('run');
            watcher.unobserve(entry.target);
        }
    });
}, { threshold: 0.4 });

watcher.observe(document.getElementById('curve'));

// A number rests on its real value and rolls back to it, so it's never wrong.
function prepareRoll(element) {
    const text = element.textContent;
    element.textContent = '';
    element.setAttribute('role', 'img');
    element.setAttribute('aria-label', text);
    element.classList.add('rolling');

    [...text].forEach((character, position) => {
        const cell = document.createElement('span');
        const reel = document.createElement('span');
        const count = /\d/.test(character) ? 4 + position * 2 : 0;

        for (let k = 0; k <= count; k++) {
            const line = document.createElement('span');
            line.style.setProperty('--k', k);
            line.textContent = k === 0 || k === count ? character : '72941683'[(k + position * 3) % 8];
            reel.appendChild(line);
        }

        cell.className = 'roll-cell';
        reel.className = 'roll-reel';
        reel.style.setProperty('--n', position);
        reel.style.setProperty('--count', count);
        cell.appendChild(reel);
        element.appendChild(cell);
    });

    watcher.observe(element);
}

// Wait for data.js, because filling a value would replace the reels.
if (!matchMedia('(prefers-reduced-motion: reduce)').matches) {
    filled.then(() => document.querySelectorAll('.score').forEach(prepareRoll));
}

// A download link lands on the version, so it rolls again every time.
document.querySelectorAll('a[href="#download"]').forEach((link) => {
    link.addEventListener('click', () => {
        const version = document.querySelector('.download .score');
        version.classList.add('run');
        version.querySelectorAll('.roll-reel').forEach((reel) => {
            reel.getAnimations().forEach((animation) => {
                animation.cancel();
                animation.play();
            });
        });
    });
});
