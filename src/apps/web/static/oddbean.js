const bech32Ctx = genBech32('bech32');

function encodeBech32(prefix, inp) {
    return bech32Ctx.encode(prefix, bech32Ctx.toWords(base16.decode(inp.toUpperCase())));
}

function decodeBech32(inp) {
    return base16.encode(bech32Ctx.fromWords(bech32Ctx.decode(inp).words)).toLowerCase();
}

document.addEventListener('alpine:init', () => {
    Alpine.data('obLogin', () => ({
        loggedIn: false,
        pubkey: '',
        username: '',

        init() {
            let storage = JSON.parse(window.localStorage.getItem('auth') || '{}');
            if (storage.pubkey) {
                this.loggedIn = true;
                this.pubkey = storage.pubkey;
                this.username = storage.username;
            }
        },

        async login() {
            let pubkey = await nostr.getPublicKey();

            let response = await fetch(`/u/${pubkey}/metadata.json`);
            let json = await response.json();

            let username = pubkey.substr(0, 8) + '...';

            this.pubkey = pubkey;
            this.username = username;
            window.localStorage.setItem('auth', JSON.stringify({ pubkey, username, }));

            this.loggedIn = true;
        },

        myProfile() {
            return `/u/${encodeBech32('npub', this.pubkey)}`;
        },

        logout() {
            window.localStorage.setItem('auth', '{}');

            this.loggedIn = false;
        },
    }));

    Alpine.data('newPost', () => ({
        init() {
        },

        async submit() {
            this.$refs.msg.innerText = '';

            let ev = {
                created_at: Math.floor(((new Date()) - 0) / 1000),
                kind: 1,
                tags: [],
                content: this.$refs.post.value,
            };

            ev = await window.nostr.signEvent(ev);

            let resp = await fetch("/submit-post", {
                method: "post",
                headers: {
                    'Accept': 'application/json',
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(ev),
            });

            let json = await resp.json();

            if (json.message === 'ok' && json.written === true) {
                window.location = `/e/${json.event}`
            } else {
                this.$refs.msg.innerText = `Sending note failed: ${json.message}`;
                console.error(json);
            }
        },
    }))
});


document.addEventListener("click", async (e) => {
    let parent = e.target.closest(".do-vote");
    if (!parent) return;

    let which = e.target.className;
    if (which.length !== 1) return;

    let note = parent.getAttribute('data-note');

    console.log({which,note});

    e.target.className = 'loading';
    e.target.innerText = '↻';
});
