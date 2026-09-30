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
import java.security.spec.MGF1ParameterSpec;
import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import javax.crypto.spec.OAEPParameterSpec;
import javax.crypto.spec.PSource;

/** Keeps the vault key device-local: only a Keystore private key can unwrap it. */
public final class VaultKeyStore {
    private static final String STORE = "p2p_messenger_vault";
    private static final String PREFIX = "vault-rsa-";
    private static final OAEPParameterSpec OAEP = new OAEPParameterSpec(
        "SHA-256", "MGF1", MGF1ParameterSpec.SHA1, PSource.PSpecified.DEFAULT);
    private VaultKeyStore() { }

    public static byte[] loadOrCreate(Context context, String name) throws Exception {
        String alias = PREFIX + name;
        SharedPreferences preferences = context.getSharedPreferences(STORE, Context.MODE_PRIVATE);
        String stored = preferences.getString(name, null);
        KeyStore keyStore = KeyStore.getInstance("AndroidKeyStore");
        keyStore.load(null);
        if (stored != null && !keyStore.containsAlias(alias))
            throw new IllegalStateException("Existing vault key is missing from Android Keystore; data was preserved");
        if (!keyStore.containsAlias(alias)) {
            KeyPairGenerator generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore");
            generator.initialize(new KeyGenParameterSpec.Builder(alias,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_RSA_OAEP)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                .build());
            generator.generateKeyPair();
        }
        Cipher cipher = Cipher.getInstance("RSA/ECB/OAEPWithSHA-256AndMGF1Padding");
        if (stored != null) {
            cipher.init(Cipher.DECRYPT_MODE, keyStore.getKey(alias, null), OAEP);
            return cipher.doFinal(Base64.decode(stored, Base64.NO_WRAP));
        }
        byte[] key = new byte[32];
        new SecureRandom().nextBytes(key);
        PublicKey publicKey = keyStore.getCertificate(alias).getPublicKey();
        cipher.init(Cipher.ENCRYPT_MODE, publicKey, OAEP);
        byte[] wrapped = cipher.doFinal(key);
        // Check the same Keystore private key can unwrap the new key before
        // storing it. Provider-default OAEP parameters differ on Android.
        cipher.init(Cipher.DECRYPT_MODE, keyStore.getKey(alias, null), OAEP);
        if (!java.util.Arrays.equals(key, cipher.doFinal(wrapped)))
            throw new IllegalStateException("Android Keystore round trip failed");
        if (!preferences.edit().putString(name, Base64.encodeToString(wrapped, Base64.NO_WRAP)).commit())
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
