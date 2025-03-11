#include <linux/i2c.h>
#include <linux/miscdevice.h>

#define ADXL345_TYPE 'A'
#define ADXL345_NO_ARG _IO(ADXL345_TYPE, 1)
#define ADXL345_READ _IOR(ADXL345_TYPE, 2, int)
#define ADXL345_WRITE _IOW(ADXL345_TYPE, 3, int)
#define ADXL345_READWRITE _IOWR(ADXL345_TYPE, 4, int)

#define BUFFER_SIZE 32
#define MAX_CONSUMERS 4

struct fifo_element
{
    __u8 data[6];
};

typedef struct {
    struct fifo_element buffer[BUFFER_SIZE];
    atomic_t queue;
    atomic_t tete[MAX_CONSUMERS];
    atomic_t consumer_id_distribues[MAX_CONSUMERS]; /*boolean*/
} RingBuffer;

static void ring_buffer_init(RingBuffer *rb) {
    atomic_set(&rb->queue, 0);
    for (__u8 i = 0; i < MAX_CONSUMERS; i++) {
        atomic_set(&rb->tete[i], 0);
        atomic_set(&rb->consumer_id_distribues[i], false);
    }
}

static int ring_buffer_get_id(RingBuffer *rb) {
    for (__u8 i = 0 ; i < MAX_CONSUMERS ; i++) {
        if (! atomic_cmpxchg(&rb->consumer_id_distribues[i], false, true)) return i;
    }
    return -1;
}

static void ring_buffer_release_id(RingBuffer *rb, __u8 consumer_id) {
    atomic_set(&rb->consumer_id_distribues[consumer_id], false);
    wmb(); /* be sure not to get before having released */
}

static void ring_buffer_push(RingBuffer *rb, struct fifo_element data) {
    __u8 queue_idx = atomic_read(&rb->queue);
    __u8 next_queue_idx = (queue_idx + 1) % BUFFER_SIZE;
    __u8 next_next_queue_idx = (queue_idx + 2) % BUFFER_SIZE;
    for (__u8 i = 0; i < MAX_CONSUMERS; i++) {
        atomic_cmpxchg(&rb->tete[i], next_queue_idx, next_next_queue_idx);
    }
    rb->buffer[queue_idx] = data;
    wmb(); /* be sure the data is written before increasing the queue index */
    atomic_set(&rb->queue, next_queue_idx);
}

static bool ring_buffer_is_empty(RingBuffer *rb, __u8 consumer_id) {
    __u8 queue = atomic_read(&rb->queue);
    rmb(); /* A false positive isn't destructive, so it's not a problem if the queue has been
 * increased after the barrier. But in the opposite order there could be a false negative if the id is skipped
 * inbetween. So we rather have queue read before tete. */
    __u8 tete = atomic_read(&rb->tete[consumer_id]);
    return queue == tete;
}

static bool ring_buffer_pop(RingBuffer *rb, __u8 consumer_id, struct fifo_element *data) {
    if (ring_buffer_is_empty(rb, consumer_id)) return false;
    /* We assume only one thread pops consumer_id, beside the skip of ring_buffer_push for which the ring buffer
     * is obviously not empty. Hence it has to be checked once. */
    __u8 tete_idx;
    __u8 tete_idx_next;
    __u8 old_tete_idx;
    do {
        tete_idx = atomic_read(&rb->tete[consumer_id]);
        tete_idx_next = (tete_idx + 1) % BUFFER_SIZE;
        *data = rb->buffer[tete_idx];
        wmb(); /* The data has to be written in the buffer before it can be erased */
        old_tete_idx = atomic_cmpxchg(&rb->tete[consumer_id], tete_idx, tete_idx_next);
    } while (old_tete_idx != tete_idx);
    /* it is possible for tete[consumer_id] to be incremented by the push function inbetween.
     * Hence it has to be checked to avoid having a delayed index returning the newest data instead of the latest. */
    return true;
}

struct adxl345_device
{
    struct miscdevice miscdev;
    wait_queue_head_t waiting_queue;
    RingBuffer* rb;
};

struct adxl345_privatedata
{
    struct adxl345_device *adxl345_dev;
    __u8 ringbuffer_id;
    char axe_actuel;
};

static int adxl345_open(__attribute__((unused)) struct inode *inode, struct file *file)
{
    struct adxl345_privatedata *private_data;
    struct miscdevice *miscdev = file->private_data;
    struct adxl345_device *dev = container_of(miscdev, struct adxl345_device, miscdev);
    private_data = kmalloc(sizeof(struct adxl345_privatedata), GFP_KERNEL);
    if (!private_data)
        return -ENOMEM;
    private_data->axe_actuel = 'X';
    private_data->adxl345_dev = dev;
    int id = ring_buffer_get_id(dev->rb);
    if (id == -1) return -ENOMEM;
    private_data->ringbuffer_id = id;
    file->private_data = private_data;
    return 0;
}

static int adxl345_release(__attribute__((unused)) struct inode *inode, struct file *file)
{
    struct adxl345_privatedata* private_data = file->private_data;
    ring_buffer_release_id(private_data->adxl345_dev->rb, private_data->ringbuffer_id);
    kfree(file->private_data);
    return 0;
}

static long adxl345_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    char axe = ((struct adxl345_privatedata *)(file->private_data))->axe_actuel;
    switch (cmd)
    {
    case ADXL345_NO_ARG:
        pr_warn("ADXL345 appelé en ioctl sans argument\n");
        break;

    case ADXL345_READ:
        if (copy_to_user((unsigned long __user *)arg, &axe, sizeof(axe)))
        {
            return -EFAULT;
        }
        break;

    case ADXL345_WRITE:
    case ADXL345_READWRITE:
        if (copy_from_user(&axe, (unsigned long __user *)arg, sizeof(axe)))
        {
            return -EFAULT;
        }
        ((struct adxl345_privatedata *)(file->private_data))->axe_actuel = axe;
        break;

    default:
        return -ENOTTY;
    }

    return 0;
}

static void lecture_registre_multibyte(struct i2c_client *client, __u8 adresse, __u8 *buffer, size_t length)
{
    struct i2c_msg msgs[2];
    int ret;
    msgs[0].addr  = client->addr;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &adresse;
    msgs[1].addr  = client->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = length;
    msgs[1].buf   = buffer;
    ret = i2c_transfer(client->adapter, msgs, 2);
    if (ret < 0) {
        pr_err("Erreur lors du transfert i2c de lecture de registre : %d\n", ret);
    }
}

static __u8 lecture_registre(struct i2c_client *client, __u8 adresse)
{
    __u8 ret;
    lecture_registre_multibyte(client, adresse, &ret, 1);
    return ret;
}

static void ecriture_registre(struct i2c_client *client, __u8 adresse, __u8 valeur)
{
    __u8 buffer[2];
    buffer[0] = adresse;
    buffer[1] = valeur;
    i2c_master_send(client, buffer, 2);
}

static void affiche_registre(struct i2c_client *client, __u8 adresse)
{
    __u8 regval = lecture_registre(client, adresse);
    pr_info("Valeur du registre : %02X\n", regval);
}

static ssize_t adxl345_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
    struct adxl345_privatedata *private_data = (struct adxl345_privatedata *)file->private_data;
    *f_pos = 0; /* C'est un capteur on n'a pas de position */
    count = min(2, count);
    __u8 buf_kernel[2];
    __u8 reg_0, reg_1;
    switch (private_data->axe_actuel)
    {
    case 'Y':
        reg_1 = 3;
        reg_0 = 2;
        break;
    case 'Z':
        reg_1 = 5;
        reg_0 = 4;
        break;
    case 'X':
    default:
        reg_1 = 1;
        reg_0 = 0;
    }
    struct fifo_element elem;
    int ret;
    struct adxl345_device *dev = private_data->adxl345_dev;
    while (ring_buffer_is_empty(dev->rb, private_data->ringbuffer_id))
    {
        ret = wait_event_interruptible(dev->waiting_queue, !ring_buffer_is_empty(dev->rb, private_data->ringbuffer_id));
        if (ret == -ERESTARTSYS) return -EINTR;
    }
    ret = ring_buffer_pop(dev->rb, private_data->ringbuffer_id, &elem);
    if (! ret) return -EIO; // Normalement impossible
    if (count > 0)
        buf_kernel[0] = elem.data[reg_0];
    if (count > 1)
        buf_kernel[1] = elem.data[reg_1];
    if (copy_to_user(buf, buf_kernel, count))
        return -EFAULT;
    return (ssize_t) count;
}

static irqreturn_t adxl345_int(__attribute__((unused)) int irq, void *dev_id)
{
    struct adxl345_device *dev = dev_id;
    struct i2c_client *client = to_i2c_client(dev->miscdev.parent);
    __u8 n_ech = lecture_registre(client, 0x39) & 0b111111;
    struct fifo_element element;
    for (int i = 0; i < n_ech; i++)
    {
        lecture_registre_multibyte(client, 0x32, element.data, 6);
        ring_buffer_push(dev->rb, element);
    }
    wake_up(&dev->waiting_queue);
    return IRQ_HANDLED;
}

static const struct file_operations adxl345_fops = {
    .owner = THIS_MODULE,
    .read = adxl345_read,
    .unlocked_ioctl = adxl345_unlocked_ioctl,
    .open = adxl345_open,
    .release = adxl345_release
};

static int adxl345_probe(struct i2c_client *client)
{
    static atomic_t n_instances = ATOMIC_INIT(0);
    char *nom = kasprintf(GFP_KERNEL, "adxl345-%d", atomic_read(&n_instances));
    if (!nom)
        return -ENOMEM;
    struct adxl345_device *dev = kmalloc(sizeof(struct adxl345_device), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    atomic_inc(&n_instances);
    pr_info("Adxl345 connecté!\n");
    i2c_set_clientdata(client, dev);
    dev->miscdev.minor = MISC_DYNAMIC_MINOR;
    dev->miscdev.name = nom;
    dev->miscdev.parent = &client->dev;
    dev->miscdev.fops = &adxl345_fops;
    dev->miscdev.groups = NULL;
    dev->miscdev.nodename = NULL;
    dev->miscdev.this_device = NULL;
    int ret = misc_register(&(dev->miscdev));
    if (ret)
    {
        pr_err("Failed to register misc device\n");
        kfree(nom);
        kfree(dev);
        return ret;
    }
    ecriture_registre(client, 0x2C, 0xA); /*BW_RATE output data rate 100Hz*/
    affiche_registre(client, 0x2C);
    ecriture_registre(client, 0x2E, 0x2); /*INT_ENABLE interruption WATERMARK*/
    affiche_registre(client, 0x2E);
    ecriture_registre(client, 0x31, 0x0); /*DATA_FORMAT default*/
    affiche_registre(client, 0x31);
    ecriture_registre(client, 0x38, 0b10010100); /*FIFO_CTL Mode stream et n_samples = 20*/
    affiche_registre(client, 0x38);
    ecriture_registre(client, 0x2D, 0xA); /*POWER_CTL pas de standby*/
    affiche_registre(client, 0x2D);
    dev->rb = kmalloc(sizeof(RingBuffer), GFP_KERNEL);
    ring_buffer_init(dev->rb);
    init_waitqueue_head(&dev->waiting_queue);
    ret = devm_request_threaded_irq(&client->dev, client->irq, NULL, adxl345_int, IRQF_ONESHOT, nom, dev);
    if (ret)
    {
        pr_err("Echec de la requête IRQ\n");
        misc_deregister(&(dev->miscdev));
        kfree(nom);
        kfree(dev);
        return ret;
    }
    return 0;
}

static void adxl345_remove(struct i2c_client *client)
{
    pr_info("Adxl345 retiré!\n");
    struct adxl345_device *dev = i2c_get_clientdata(client);
    ecriture_registre(client, 0x2D, 0x2); /*POWER_CTL standby*/
    affiche_registre(client, 0x2D);
    misc_deregister(&(dev->miscdev));
    kfree(dev->rb);
    kfree(dev->miscdev.name);
    kfree(dev);
}

/* La liste suivante permet l'association entre un périphérique et son
 *    pilote dans le cas d'une initialisation statique sans utilisation de
 *       device tree.
 *
 *          Chaque entrée contient une chaîne de caractère utilisée pour
 *             faire l'association et un entier qui peut être utilisé par le
 *                pilote pour effectuer des traitements différents en fonction
 *                   du périphérique physique détecté (cas d'un pilote pouvant gérer
 *                      différents modèles de périphérique).
 *                      */
static struct i2c_device_id adxl345_idtable[] = {
    {"adxl345", 0},
    {}};
MODULE_DEVICE_TABLE(i2c, adxl345_idtable);

#ifdef CONFIG_OF
/* Si le support des device trees est disponible, la liste suivante
 *    permet de faire l'association à l'aide du device tree.
 *
 *       Chaque entrée contient une structure de type of_device_id. Le champ
 *          compatible est une chaîne qui est utilisée pour faire l'association
 *             avec les champs compatible dans le device tree. Le champ data est
 *                un pointeur void* qui peut être utilisé par le pilote pour
 *                   effectuer des traitements différents en fonction du périphérique
 *                      physique détecté.
 *                      */
static const struct of_device_id adxl345_of_match[] = {
    {.compatible = "qemu,adxl345",
     .data = NULL},
    {}};

MODULE_DEVICE_TABLE(of, adxl345_of_match);
#endif

static struct i2c_driver adxl345_driver = {
    .driver = {
        /* Le champ name doit correspondre au nom du module
         *            et ne doit pas contenir d'espace */
        .name = "adxl345",
        .of_match_table = of_match_ptr(adxl345_of_match),
    },

    .id_table = adxl345_idtable,
    .probe = adxl345_probe,
    .remove = adxl345_remove,
};

module_i2c_driver(adxl345_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Adxl345 driver");
MODULE_AUTHOR("Arnaud");
