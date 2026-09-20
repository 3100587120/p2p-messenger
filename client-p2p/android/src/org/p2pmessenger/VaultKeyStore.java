package org.p2pmessenger;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PublicKey;
import java.security.SecureRandom;
import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/** Keeps the vault key device-local: only a Keystore private key can unwrap it. */
public final class VaultKeyStore {
    private static final String STORE = "p2p_messenger_vault";
    private static final String PREFIX = "vault-rsa-";
    private VaultKeyStore() { }

    public static byte[] loadOrCreate(Context context, String name) throws Exception {
        String alias = PREFIX + name;
        SharedPreferences preferences = context.getSharedPreferences(STORE, Context.MODE_PRIVATE);
        KeyStore keyStore = KeyStore.getInstance("AndroidKeyStore");
        keyStore.load(null);
        if (!keyStore.containsAlias(alias)) {
            KeyPairGenerator generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore");
            generator.initialize(new KeyGenParameterSpec.Builder(alias,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_RSA_OAEP)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                .build());
            generator.generateKeyPair();
        }
        String stored = preferences.getString(name, null);
        Cipher cipher = Cipher.getInstance("RSA/ECB/OAEPWithSHA-256AndMGF1Padding");
        if (stored != null) {
            cipher.init(Cipher.DECRYPT_MODE, keyStore.getKey(alias, null));
            return cipher.doFinal(Base64.decode(stored, Base64.NO_WRAP));
        }
        byte[] key = new byte[32];
        new SecureRandom().nextBytes(key);
        PublicKey publicKey = keyStore.getCertificate(alias).getPublicKey();
        cipher.init(Cipher.ENCRYPT_MODE, publicKey);
        if (!preferences.edit().putString(name, Base64.encodeToString(cipher.doFinal(key), Base64.NO_WRAP)).commit())
            throw new IllegalStateException("Cannot save wrapped vault key");
        return key;
    }

    public static byte[] encrypt(byte[] key, byte[] nonce, byte[] input) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, nonce));
        return cipher.doFinal(input);
    }

    public static byte[] decrypt(byte[] key, byte[] nonce, byte[] input) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, nonce));
        return cipher.doFinal(input);
    }
}
