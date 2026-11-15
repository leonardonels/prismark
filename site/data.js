// Which package the main button offers. A browser can't tell .deb from .rpm, and there's no package
// for an iPhone or iPad (which can call itself a Mac), so those go to the list.
function systemPackage() {
    const agent = navigator.userAgent;
    if (/iPhone|iPad|iPod/.test(agent) || navigator.maxTouchPoints > 1 && /Macintosh/.test(agent)) return null;
    if (/Android/.test(agent)) return { key: 'android', label: 'ANDROID ⟶' };
    if (/Windows/.test(agent)) return { key: 'windows', label: 'WINDOWS ⟶' };
    if (/Macintosh/.test(agent)) return { key: 'macos', label: 'MACOS ⟶' };
    return null;
}

function fillDownloads(downloads) {
    if (!downloads) return;
    document.querySelectorAll('[data-download]').forEach((link) => {
        link.href = downloads[link.dataset.download];
    });
    const main = document.getElementById('get');
    if (!main) return;
    const offer = systemPackage();
    main.href = offer ? downloads[offer.key] : '#platforms';
    main.querySelector('.label').textContent = offer ? offer.label : 'ALL FILES ⟶';
}

// The ends of the package names in a release, as PACKAGES in tools/site_data.py: change both together.
const PACKAGES = {
    macos: '-macos-arm64.dmg',
    windows: '-windows-x86_64.zip',
    'deb-x86_64': '_amd64.deb',
    'deb-arm64': '_arm64.deb',
    'rpm-x86_64': '.x86_64.rpm',
    'rpm-arm64': '.aarch64.rpm',
    android: '-android-arm64-release.apk',
    ios: '-ios-arm64-unsigned.ipa',
};

// The newest published release, asked of GitHub on each visit. The site is rebuilt when a release is published, but
// Pages keeps serving the deployment already live for the same commit, and a release is often tagged on the commit
// main last deployed: data.json then still names the release before. Null when GitHub doesn't answer or the release
// lacks a package, and data.json stands.
function newestRelease() {
    return fetch('https://api.github.com/repos/leonardonels/prismark/releases?per_page=10',
                 { headers: { Accept: 'application/vnd.github+json' } })
        .then((response) => (response.ok ? response.json() : []))
        .then((releases) => {
            const release = releases.find((r) => !r.draft);
            if (!release) return null;
            const downloads = {};
            for (const [key, ending] of Object.entries(PACKAGES)) {
                const files = release.assets.filter((a) => a.name.endsWith(ending));
                if (files.length !== 1) return null;
                downloads[key] = files[0].browser_download_url;
            }
            return { version: release.tag_name.replace(/^v/, ''), downloads };
        })
        .catch(() => null);
}

// no-cache: the browser asks again every visit.
const filled = Promise.all([
    fetch('data.json', { cache: 'no-cache' }).then((response) => response.json()).catch(() => null),
    newestRelease(),
]).then(([data, release]) => {
    // No file and no answer: the values already in the HTML stay on the page.
    data = Object.assign(data || {}, release || {});
    document.querySelectorAll('[data-fill]').forEach((element) => {
        const value = data[element.dataset.fill];
        if (value) element.textContent = value;
    });
    fillDownloads(data.downloads);
});
